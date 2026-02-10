#include "tcp_accept_watcher.h"

#ifdef __linux__

#include <cerrno>

#include "../../schedule/epoll_backend.h"
#include "../../schedule/poll_backend.h"

#include "tcp_socket_common.h"

namespace RopHive::Linux {
namespace {

using namespace RopHive::Network;

static int createListenSocket(const TcpAcceptOption &option) {
  sockaddr_storage ss{};
  socklen_t ss_len = 0;
  if (!TcpDetail::toSockaddr(option.local, ss, ss_len)) {
    errno = EINVAL;
    return -1;
  }

  const int family = reinterpret_cast<sockaddr *>(&ss)->sa_family;
  const int type_flags =
      SOCK_STREAM | SOCK_NONBLOCK |
      (option.set_close_on_exec.has_value() && *option.set_close_on_exec
           ? SOCK_CLOEXEC
           : 0);

  int fd = ::socket(family, type_flags, 0);
  if (fd < 0)
    return -1;

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

class EpollTcpAcceptWatcher final : public ITcpAcceptWatcher {
public:
  EpollTcpAcceptWatcher(IOWorker &worker, int listen_fd, TcpAcceptOption option,
                        OnAccept on_accept, OnError on_error)
      : ITcpAcceptWatcher(worker), listen_fd_(listen_fd),
        option_(std::move(option)), on_accept_(std::move(on_accept)),
        on_error_(std::move(on_error)) {
    source_ = std::make_shared<RopHive::Linux::EpollReadinessEventSource>(
        listen_fd_, EPOLLIN | EPOLLERR | EPOLLHUP,
        [this](uint32_t events) { onReady(events); });
  }

  ~EpollTcpAcceptWatcher() override {
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
  void onReady(uint32_t events) {
    if (listen_fd_ < 0)
      return;

    if (events & (EPOLLERR | EPOLLHUP)) {
      int err = 0;
      socklen_t len = sizeof(err);
      if (::getsockopt(listen_fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        err = errno;
      }
      if (on_error_)
        on_error_(err != 0 ? err : EIO);
      return;
    }

    if (events & EPOLLIN) {
      acceptLoop();
    }
  }

  void acceptLoop() {
    const int accept_flags =
        SOCK_NONBLOCK |
        (option_.set_close_on_exec.has_value() && *option_.set_close_on_exec
             ? SOCK_CLOEXEC
             : 0);

    size_t accepted = 0;
    while (accepted < option_.max_accept_per_tick) {
      sockaddr_storage peer{};
      socklen_t peer_len = sizeof(peer);
      int client_fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr *>(&peer),
                                &peer_len, accept_flags);
      if (client_fd >= 0) {
        accepted++;

        TcpDetail::applyCloseOnExecIfConfigured(client_fd,
                                                option_.set_close_on_exec);
        TcpDetail::applyTcpNoDelayIfConfigured(client_fd, option_.tcp_no_delay);
        TcpDetail::applyKeepAliveIfConfigured(
            client_fd, option_.keep_alive, option_.keep_alive_idle_sec,
            option_.keep_alive_interval_sec, option_.keep_alive_count);
        TcpDetail::applyBufSizeIfConfigured(client_fd, option_.recv_buf_bytes,
                                            option_.send_buf_bytes);

        auto stream = std::make_unique<LinuxTcpStream>(
            TcpStreamKind::LinuxEpoll, client_fd);
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
  std::shared_ptr<RopHive::Linux::EpollReadinessEventSource> source_;
};

class PollTcpAcceptWatcher final : public ITcpAcceptWatcher {
public:
  PollTcpAcceptWatcher(IOWorker &worker, int listen_fd, TcpAcceptOption option,
                       OnAccept on_accept, OnError on_error)
      : ITcpAcceptWatcher(worker), listen_fd_(listen_fd),
        option_(std::move(option)), on_accept_(std::move(on_accept)),
        on_error_(std::move(on_error)) {
    source_ = std::make_shared<RopHive::Linux::PollReadinessEventSource>(
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
      int err = 0;
      socklen_t len = sizeof(err);
      if (::getsockopt(listen_fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        err = errno;
      }
      if (on_error_)
        on_error_(err != 0 ? err : EIO);
      return;
    }

    if (revents & POLLIN) {
      acceptLoop();
    }
  }

  void acceptLoop() {
    const int accept_flags =
        SOCK_NONBLOCK |
        (option_.set_close_on_exec.has_value() && *option_.set_close_on_exec
             ? SOCK_CLOEXEC
             : 0);

    size_t accepted = 0;
    while (accepted < option_.max_accept_per_tick) {
      sockaddr_storage peer{};
      socklen_t peer_len = sizeof(peer);
      int client_fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr *>(&peer),
                                &peer_len, accept_flags);
      if (client_fd >= 0) {
        accepted++;

        TcpDetail::applyCloseOnExecIfConfigured(client_fd,
                                                option_.set_close_on_exec);
        TcpDetail::applyTcpNoDelayIfConfigured(client_fd, option_.tcp_no_delay);
        TcpDetail::applyKeepAliveIfConfigured(
            client_fd, option_.keep_alive, option_.keep_alive_idle_sec,
            option_.keep_alive_interval_sec, option_.keep_alive_count);
        TcpDetail::applyBufSizeIfConfigured(client_fd, option_.recv_buf_bytes,
                                            option_.send_buf_bytes);

        auto stream = std::make_unique<LinuxTcpStream>(TcpStreamKind::LinuxPoll,
                                                       client_fd);
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
  std::shared_ptr<RopHive::Linux::PollReadinessEventSource> source_;
};

} // namespace

std::shared_ptr<RopHive::Network::ITcpAcceptWatcher>
createEpollTcpAcceptWatcher(IOWorker &worker, TcpAcceptOption option,
                            ITcpAcceptWatcher::OnAccept on_accept,
                            ITcpAcceptWatcher::OnError on_error) {
  const int listen_fd = createListenSocket(option);
  if (listen_fd < 0) {
    if (on_error)
      on_error(errno);
    return {};
  }
  return std::make_shared<EpollTcpAcceptWatcher>(
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

} // namespace RopHive::Linux

#endif // __linux__
