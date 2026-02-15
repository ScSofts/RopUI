#include "tcp_accept_watcher.h"

#ifdef __APPLE__

#include <cerrno>

#include <sys/event.h>

#include "../../schedule/kqueue_backend.h"
#include "../../schedule/poll_backend.h"

#include "tcp_socket_common.h"

namespace RopHive::MacOS {
namespace {

using namespace RopHive::Network;

static int getSoErrorOr(int fd, int fallback) {
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
    return errno != 0 ? errno : fallback;
  }
  return err != 0 ? err : fallback;
}

static int createListenSocket(const TcpAcceptOption &option) {
  sockaddr_storage ss{};
  socklen_t ss_len = 0;
  if (!TcpDetail::toSockaddr(option.local, ss, ss_len)) {
    errno = EINVAL;
    return -1;
  }

  const int family = reinterpret_cast<sockaddr *>(&ss)->sa_family;
  int fd = ::socket(family, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  if (!TcpDetail::setNonBlocking(fd)) {
    TcpDetail::closeFd(fd);
    return -1;
  }

  TcpDetail::applyCloseOnExecIfConfigured(fd, option.set_close_on_exec);
  TcpDetail::applyReuseAddrIfConfigured(fd, option.reuse_addr);
  TcpDetail::applyReusePortIfConfigured(fd, option.reuse_port);
  if (family == AF_INET6) {
    TcpDetail::applyIpv6OnlyIfConfigured(fd, option.ipv6_only);
  }

  if (::bind(fd, reinterpret_cast<sockaddr *>(&ss), ss_len) != 0) {
    TcpDetail::closeFd(fd);
    return -1;
  }
  if (::listen(fd, option.backlog) != 0) {
    TcpDetail::closeFd(fd);
    return -1;
  }
  return fd;
}

static bool bestEffortPrepareAcceptedSocket(int client_fd,
                                            const TcpAcceptOption &option) {
  if (!TcpDetail::setNonBlocking(client_fd)) {
    return false;
  }
  TcpDetail::applyCloseOnExecIfConfigured(client_fd, option.set_close_on_exec);
  TcpDetail::applyNoSigPipeIfAvailable(client_fd);
  TcpDetail::applyTcpNoDelayIfConfigured(client_fd, option.tcp_no_delay);
  TcpDetail::applyKeepAliveIfConfigured(
      client_fd, option.keep_alive, option.keep_alive_idle_sec,
      option.keep_alive_interval_sec, option.keep_alive_count);
  TcpDetail::applyBufSizeIfConfigured(client_fd, option.recv_buf_bytes,
                                      option.send_buf_bytes);
  return true;
}

class KqueueTcpAcceptWatcher final : public ITcpAcceptWatcher {
public:
  KqueueTcpAcceptWatcher(IOWorker &worker, int listen_fd, TcpAcceptOption option,
                         OnAccept on_accept, OnError on_error)
      : ITcpAcceptWatcher(worker), listen_fd_(listen_fd),
        option_(std::move(option)), on_accept_(std::move(on_accept)),
        on_error_(std::move(on_error)) {
    source_ = std::make_shared<RopHive::MacOS::KqueueReadinessEventSource>(
        listen_fd_, EVFILT_READ,
        [this](const RopHive::MacOS::KqueueRawEvent &ev) { onReady(ev); });
  }

  ~KqueueTcpAcceptWatcher() override {
    stop();
    source_.reset();
    TcpDetail::closeFd(listen_fd_);
  }

  void start() override {
    if (attached_)
      return;
    attachSource(source_);
    attached_ = true;
  }

  void stop() override {
    if (!attached_)
      return;
    detachSource(source_);
    attached_ = false;
  }

private:
  void onReady(const RopHive::MacOS::KqueueRawEvent &ev) {
    if (listen_fd_ < 0)
      return;

    if (ev.flags & (EV_ERROR | EV_EOF)) {
      const int err =
          (ev.flags & EV_ERROR) ? static_cast<int>(ev.data)
                                : getSoErrorOr(listen_fd_, EIO);
      if (on_error_)
        on_error_(err != 0 ? err : EIO);
      return;
    }

    acceptLoop(TcpStreamKind::MacKqueue);
  }

  void acceptLoop(TcpStreamKind kind) {
    size_t accepted = 0;
    while (accepted < option_.max_accept_per_tick) {
      sockaddr_storage peer{};
      socklen_t peer_len = sizeof(peer);
      int client_fd =
          ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&peer), &peer_len);
      if (client_fd >= 0) {
        if (!bestEffortPrepareAcceptedSocket(client_fd, option_)) {
          TcpDetail::closeFd(client_fd);
          if (on_error_)
            on_error_(errno != 0 ? errno : EIO);
          return;
        }

        accepted++;
        auto stream = std::make_unique<MacTcpStream>(kind, client_fd);
        if (option_.fill_endpoints) {
          stream->peer = TcpDetail::fromSockaddr(
              reinterpret_cast<sockaddr *>(&peer), peer_len);
          TcpDetail::bestEffortFillEndpoints(client_fd, *stream);
        }

        if (on_accept_)
          on_accept_(std::move(stream));
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      if (errno == EINTR)
        continue;
      if (on_error_)
        on_error_(errno);
      return;
    }
  }

private:
  int listen_fd_{-1};
  TcpAcceptOption option_;
  OnAccept on_accept_;
  OnError on_error_;

  bool attached_{false};
  std::shared_ptr<RopHive::MacOS::KqueueReadinessEventSource> source_;
};

class PollTcpAcceptWatcher final : public ITcpAcceptWatcher {
public:
  PollTcpAcceptWatcher(IOWorker &worker, int listen_fd, TcpAcceptOption option,
                       OnAccept on_accept, OnError on_error)
      : ITcpAcceptWatcher(worker), listen_fd_(listen_fd),
        option_(std::move(option)), on_accept_(std::move(on_accept)),
        on_error_(std::move(on_error)) {
    source_ = std::make_shared<RopHive::MacOS::PollReadinessEventSource>(
        listen_fd_, POLLIN, [this](short revents) { onReady(revents); });
  }

  ~PollTcpAcceptWatcher() override {
    stop();
    source_.reset();
    TcpDetail::closeFd(listen_fd_);
  }

  void start() override {
    if (attached_)
      return;
    attachSource(source_);
    attached_ = true;
  }

  void stop() override {
    if (!attached_)
      return;
    detachSource(source_);
    attached_ = false;
  }

private:
  void onReady(short revents) {
    if (listen_fd_ < 0)
      return;

    if (revents & (POLLERR | POLLHUP)) {
      const int err = getSoErrorOr(listen_fd_, EIO);
      if (on_error_)
        on_error_(err != 0 ? err : EIO);
      return;
    }

    if (revents & POLLIN) {
      acceptLoop(TcpStreamKind::MacPoll);
    }
  }

  void acceptLoop(TcpStreamKind kind) {
    size_t accepted = 0;
    while (accepted < option_.max_accept_per_tick) {
      sockaddr_storage peer{};
      socklen_t peer_len = sizeof(peer);
      int client_fd =
          ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&peer), &peer_len);
      if (client_fd >= 0) {
        if (!bestEffortPrepareAcceptedSocket(client_fd, option_)) {
          TcpDetail::closeFd(client_fd);
          if (on_error_)
            on_error_(errno != 0 ? errno : EIO);
          return;
        }

        accepted++;
        auto stream = std::make_unique<MacTcpStream>(kind, client_fd);
        if (option_.fill_endpoints) {
          stream->peer = TcpDetail::fromSockaddr(
              reinterpret_cast<sockaddr *>(&peer), peer_len);
          TcpDetail::bestEffortFillEndpoints(client_fd, *stream);
        }

        if (on_accept_)
          on_accept_(std::move(stream));
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      if (errno == EINTR)
        continue;
      if (on_error_)
        on_error_(errno);
      return;
    }
  }

private:
  int listen_fd_{-1};
  TcpAcceptOption option_;
  OnAccept on_accept_;
  OnError on_error_;

  bool attached_{false};
  std::shared_ptr<RopHive::MacOS::PollReadinessEventSource> source_;
};

} // namespace

std::shared_ptr<RopHive::Network::ITcpAcceptWatcher>
createKqueueTcpAcceptWatcher(IOWorker &worker, TcpAcceptOption option,
                             ITcpAcceptWatcher::OnAccept on_accept,
                             ITcpAcceptWatcher::OnError on_error) {
  const int listen_fd = createListenSocket(option);
  if (listen_fd < 0) {
    if (on_error)
      on_error(errno);
    return {};
  }
  return std::make_shared<KqueueTcpAcceptWatcher>(
      worker, listen_fd, std::move(option), std::move(on_accept),
      std::move(on_error));
}

std::shared_ptr<RopHive::Network::ITcpAcceptWatcher>
createPollTcpAcceptWatcher(IOWorker &worker, TcpAcceptOption option,
                           ITcpAcceptWatcher::OnAccept on_accept,
                           ITcpAcceptWatcher::OnError on_error) {
  const int listen_fd = createListenSocket(option);
  if (listen_fd < 0) {
    if (on_error)
      on_error(errno);
    return {};
  }
  return std::make_shared<PollTcpAcceptWatcher>(
      worker, listen_fd, std::move(option), std::move(on_accept),
      std::move(on_error));
}

} // namespace RopHive::MacOS

#endif // __APPLE__
