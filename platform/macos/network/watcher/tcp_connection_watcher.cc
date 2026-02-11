#include "tcp_connection_watcher.h"

#ifdef __APPLE__

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <string_view>
#include <vector>

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

static int consumeMacStream(std::unique_ptr<ITcpStream> stream) {
  auto *mac_stream = dynamic_cast<MacTcpStream *>(stream.get());
  if (!mac_stream) {
    throw std::runtime_error(
        "createTcpConnectionWatcher(macos): stream type mismatch");
  }
  const int fd = mac_stream->releaseFd();
  stream.reset();
  return fd;
}

static constexpr int kSendFlagsNoSignal =
#ifdef MSG_NOSIGNAL
    MSG_NOSIGNAL;
#else
    0;
#endif

class KqueueTcpConnectionWatcher final : public ITcpConnectionWatcher {
public:
  KqueueTcpConnectionWatcher(IOWorker &worker, TcpConnectionOption option,
                             int fd, OnRecv on_recv, OnClose on_close,
                             OnError on_error, OnSendReady on_send_ready)
      : ITcpConnectionWatcher(worker), option_(std::move(option)), fd_(fd),
        on_recv_(std::move(on_recv)), on_close_(std::move(on_close)),
        on_error_(std::move(on_error)),
        on_send_ready_(std::move(on_send_ready)) {
    in_buf_.resize(64 * 1024);
    TcpDetail::applyNoSigPipeIfAvailable(fd_);
    TcpDetail::applyTcpNoDelayIfConfigured(fd_, option_.tcp_no_delay);
    TcpDetail::applyKeepAliveIfConfigured(
        fd_, option_.keep_alive, option_.keep_alive_idle_sec,
        option_.keep_alive_interval_sec, option_.keep_alive_count);
    TcpDetail::applyBufSizeIfConfigured(fd_, option_.recv_buf_bytes,
                                        option_.send_buf_bytes);
    TcpDetail::applyLingerIfConfigured(fd_, option_.linger_sec);

    read_source_ = std::make_shared<RopHive::MacOS::KqueueReadinessEventSource>(
        fd_, EVFILT_READ,
        [this](const RopHive::MacOS::KqueueRawEvent &ev) { onReadReady(ev); });
    write_source_ =
        std::make_shared<RopHive::MacOS::KqueueReadinessEventSource>(
            fd_, EVFILT_WRITE,
            [this](const RopHive::MacOS::KqueueRawEvent &ev) {
              onWriteReady(ev);
            });
  }

  ~KqueueTcpConnectionWatcher() override {
    stop();
    read_source_.reset();
    write_source_.reset();
    TcpDetail::closeFd(fd_);
  }

  void start() override {
    if (attached_)
      return;
    attachSource(read_source_);
    attached_ = true;
  }

  void stop() override {
    if (!attached_)
      return;
    if (write_attached_) {
      detachSource(write_source_);
      write_attached_ = false;
    }
    detachSource(read_source_);
    attached_ = false;
  }

  TrySendResult trySend(std::string_view data) override {
    TrySendResult res;
    if (fd_ < 0) {
      res.err = EBADF;
      return res;
    }
    if (data.empty())
      return res;

    const size_t limit =
        std::min(option_.max_write_bytes_per_tick, data.size());
    size_t total = 0;
    while (total < limit) {
      const ssize_t n =
          ::send(fd_, data.data() + total, limit - total, kSendFlagsNoSignal);
      if (n > 0) {
        total += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        want_send_ready_ = true;
        armWritable();
        res.would_block = true;
        break;
      }
      res.err = (n < 0) ? errno : EIO;
      if (on_error_)
        on_error_(res.err);
      closeNow();
      res.n = total;
      return res;
    }
    res.n = total;
    return res;
  }

  void shutdownWrite() override {
    if (fd_ < 0)
      return;
    ::shutdown(fd_, SHUT_WR);
  }

  void close() override { closeNow(); }

private:
  void armWritable() {
    if (!write_source_ || write_attached_)
      return;
    attachSource(write_source_);
    write_attached_ = true;
  }

  void disarmWritable() {
    if (!write_source_ || !write_attached_)
      return;
    detachSource(write_source_);
    write_attached_ = false;
  }

  void onReadReady(const RopHive::MacOS::KqueueRawEvent &ev) {
    if (fd_ < 0)
      return;

    if (ev.flags & EV_ERROR) {
      const int err = static_cast<int>(ev.data) != 0 ? static_cast<int>(ev.data)
                                                     : getSoErrorOr(fd_, EIO);
      if (on_error_)
        on_error_(err);
      closeNow();
      return;
    }

    if (ev.flags & EV_EOF) {
      peer_closed_ = true;
    }

    handleRead();
    if (peer_closed_) {
      closeNow();
    }
  }

  void onWriteReady(const RopHive::MacOS::KqueueRawEvent &ev) {
    if (fd_ < 0)
      return;

    if (ev.flags & EV_ERROR) {
      const int err = static_cast<int>(ev.data) != 0 ? static_cast<int>(ev.data)
                                                     : getSoErrorOr(fd_, EIO);
      if (on_error_)
        on_error_(err);
      closeNow();
      return;
    }

    if (ev.flags & EV_EOF) {
      peer_closed_ = true;
      closeNow();
      return;
    }

    if (!want_send_ready_)
      return;
    want_send_ready_ = false;
    disarmWritable();
    if (on_send_ready_)
      on_send_ready_();
  }

  void handleRead() {
    size_t remaining = option_.max_read_bytes_per_tick;
    while (remaining > 0) {
      const size_t to_read = std::min(remaining, in_buf_.size());
      const ssize_t n = ::recv(fd_, in_buf_.data(), to_read, 0);
      if (n > 0) {
        remaining -= static_cast<size_t>(n);
        if (on_recv_)
          on_recv_(std::string_view(in_buf_.data(), static_cast<size_t>(n)));
        continue;
      }
      if (n == 0) {
        peer_closed_ = true;
        return;
      }
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      if (on_error_)
        on_error_(errno);
      closeNow();
      return;
    }
  }

  void closeNow() {
    if (closed_)
      return;
    closed_ = true;
    stop();
    TcpDetail::closeFd(fd_);
    if (on_close_)
      on_close_();
  }

private:
  TcpConnectionOption option_;
  int fd_{-1};

  OnRecv on_recv_;
  OnClose on_close_;
  OnError on_error_;
  OnSendReady on_send_ready_;

  bool attached_{false};
  bool peer_closed_{false};
  bool want_send_ready_{false};
  bool closed_{false};
  bool write_attached_{false};

  std::vector<char> in_buf_;
  std::shared_ptr<RopHive::MacOS::KqueueReadinessEventSource> read_source_;
  std::shared_ptr<RopHive::MacOS::KqueueReadinessEventSource> write_source_;
};

class PollTcpConnectionWatcher final : public ITcpConnectionWatcher {
public:
  PollTcpConnectionWatcher(IOWorker &worker, TcpConnectionOption option, int fd,
                           OnRecv on_recv, OnClose on_close, OnError on_error,
                           OnSendReady on_send_ready)
      : ITcpConnectionWatcher(worker), option_(std::move(option)), fd_(fd),
        on_recv_(std::move(on_recv)), on_close_(std::move(on_close)),
        on_error_(std::move(on_error)),
        on_send_ready_(std::move(on_send_ready)) {
    in_buf_.resize(64 * 1024);
    TcpDetail::applyNoSigPipeIfAvailable(fd_);
    TcpDetail::applyTcpNoDelayIfConfigured(fd_, option_.tcp_no_delay);
    TcpDetail::applyKeepAliveIfConfigured(
        fd_, option_.keep_alive, option_.keep_alive_idle_sec,
        option_.keep_alive_interval_sec, option_.keep_alive_count);
    TcpDetail::applyBufSizeIfConfigured(fd_, option_.recv_buf_bytes,
                                        option_.send_buf_bytes);
    TcpDetail::applyLingerIfConfigured(fd_, option_.linger_sec);

    source_ = std::make_shared<RopHive::MacOS::PollReadinessEventSource>(
        fd_, POLLIN | POLLERR | POLLHUP,
        [this](short revents) { onReady(revents); });
    armed_events_ = POLLIN | POLLERR | POLLHUP;
  }

  ~PollTcpConnectionWatcher() override {
    stop();
    source_.reset();
    TcpDetail::closeFd(fd_);
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

  TrySendResult trySend(std::string_view data) override {
    TrySendResult res;
    if (fd_ < 0) {
      res.err = EBADF;
      return res;
    }
    if (data.empty())
      return res;

    const size_t limit =
        std::min(option_.max_write_bytes_per_tick, data.size());
    size_t total = 0;
    while (total < limit) {
      const ssize_t n =
          ::send(fd_, data.data() + total, limit - total, kSendFlagsNoSignal);
      if (n > 0) {
        total += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        want_send_ready_ = true;
        armWritable();
        res.would_block = true;
        break;
      }
      res.err = (n < 0) ? errno : EIO;
      if (on_error_)
        on_error_(res.err);
      closeNow();
      res.n = total;
      return res;
    }
    res.n = total;
    return res;
  }

  void shutdownWrite() override {
    if (fd_ < 0)
      return;
    ::shutdown(fd_, SHUT_WR);
  }

  void close() override { closeNow(); }

private:
  void armWritable() {
    if (!source_)
      return;
    const int want = armed_events_ | POLLOUT | POLLIN | POLLERR | POLLHUP;
    if (want == armed_events_)
      return;
    armed_events_ = want;
    source_->setEvents(static_cast<short>(armed_events_));
  }

  void disarmWritable() {
    if (!source_)
      return;
    const int want = (armed_events_ & ~POLLOUT) | POLLIN | POLLERR | POLLHUP;
    if (want == armed_events_)
      return;
    armed_events_ = want;
    source_->setEvents(static_cast<short>(armed_events_));
  }

  void onReady(short revents) {
    if (fd_ < 0)
      return;

    if (revents & POLLERR) {
      const int err = getSoErrorOr(fd_, EIO);
      if (on_error_)
        on_error_(err != 0 ? err : EIO);
      closeNow();
      return;
    }

    if (revents & POLLHUP) {
      peer_closed_ = true;
    }

    if ((revents & POLLIN) || peer_closed_) {
      handleRead();
    }
    if (peer_closed_) {
      closeNow();
      return;
    }

    if ((revents & POLLOUT) && want_send_ready_) {
      want_send_ready_ = false;
      disarmWritable();
      if (on_send_ready_)
        on_send_ready_();
    }
  }

  void handleRead() {
    size_t remaining = option_.max_read_bytes_per_tick;
    while (remaining > 0) {
      const size_t to_read = std::min(remaining, in_buf_.size());
      const ssize_t n = ::recv(fd_, in_buf_.data(), to_read, 0);
      if (n > 0) {
        remaining -= static_cast<size_t>(n);
        if (on_recv_)
          on_recv_(std::string_view(in_buf_.data(), static_cast<size_t>(n)));
        continue;
      }
      if (n == 0) {
        peer_closed_ = true;
        return;
      }
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      if (on_error_)
        on_error_(errno);
      closeNow();
      return;
    }
  }

  void closeNow() {
    if (closed_)
      return;
    closed_ = true;
    stop();
    TcpDetail::closeFd(fd_);
    if (on_close_)
      on_close_();
  }

private:
  TcpConnectionOption option_;
  int fd_{-1};

  OnRecv on_recv_;
  OnClose on_close_;
  OnError on_error_;
  OnSendReady on_send_ready_;

  bool attached_{false};
  bool peer_closed_{false};
  bool want_send_ready_{false};
  bool closed_{false};

  int armed_events_{0};
  std::vector<char> in_buf_;
  std::shared_ptr<RopHive::MacOS::PollReadinessEventSource> source_;
};

} // namespace

std::shared_ptr<RopHive::Network::ITcpConnectionWatcher>
createKqueueTcpConnectionWatcher(
    IOWorker &worker, TcpConnectionOption option,
    std::unique_ptr<ITcpStream> connected_stream,
    ITcpConnectionWatcher::OnRecv on_recv,
    ITcpConnectionWatcher::OnClose on_close,
    ITcpConnectionWatcher::OnError on_error,
    ITcpConnectionWatcher::OnSendReady on_send_ready) {
  const int fd = consumeMacStream(std::move(connected_stream));
  return std::make_shared<KqueueTcpConnectionWatcher>(
      worker, std::move(option), fd, std::move(on_recv), std::move(on_close),
      std::move(on_error), std::move(on_send_ready));
}

std::shared_ptr<RopHive::Network::ITcpConnectionWatcher>
createPollTcpConnectionWatcher(
    IOWorker &worker, TcpConnectionOption option,
    std::unique_ptr<ITcpStream> connected_stream,
    ITcpConnectionWatcher::OnRecv on_recv,
    ITcpConnectionWatcher::OnClose on_close,
    ITcpConnectionWatcher::OnError on_error,
    ITcpConnectionWatcher::OnSendReady on_send_ready) {
  const int fd = consumeMacStream(std::move(connected_stream));
  return std::make_shared<PollTcpConnectionWatcher>(
      worker, std::move(option), fd, std::move(on_recv), std::move(on_close),
      std::move(on_error), std::move(on_send_ready));
}

} // namespace RopHive::MacOS

#endif // __APPLE__
