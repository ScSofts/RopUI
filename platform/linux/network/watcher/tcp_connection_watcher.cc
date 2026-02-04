#include "tcp_connection_watcher.h"

#ifdef __linux__

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

#include "../../schedule/epoll_backend.h"
#include "../../schedule/poll_backend.h"

namespace RopHive::Linux {

static void closeFd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

EpollTcpConnectionWatcher::EpollTcpConnectionWatcher(IOWorker& worker,
                                                     int fd,
                                                     OnData on_data,
                                                     OnClose on_close,
                                                     OnError on_error)
    : IWorkerWatcher(worker),
      fd_(fd),
      on_data_(std::move(on_data)),
      on_close_(std::move(on_close)),
      on_error_(std::move(on_error)) {
    createSource(EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
}

EpollTcpConnectionWatcher::~EpollTcpConnectionWatcher() {
    stop();
    source_.reset();
    closeFd(fd_);
}

void EpollTcpConnectionWatcher::start() {
    if (attached_) return;
    attachSource(source_);
    attached_ = true;
}

void EpollTcpConnectionWatcher::stop() {
    if (!attached_) return;
    detachSource(source_);
    attached_ = false;
}

void EpollTcpConnectionWatcher::createSource(uint32_t events) {
    armed_events_ = events;
    source_ = std::make_shared<EpollReadinessEventSource>(
        fd_,
        events,
        [this](uint32_t ev) { onReady(ev); });
}

void EpollTcpConnectionWatcher::updateEvents(uint32_t events) {
    if (!source_) return;
    if (events == armed_events_) return;
    armed_events_ = events;
    source_->setEvents(events);
}

void EpollTcpConnectionWatcher::send(std::string_view data) {
    if (fd_ < 0) return;
    out_.append(data.data(), data.size());
    updateEvents(armed_events_ | EPOLLOUT | EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
    handleWrite();
}

void EpollTcpConnectionWatcher::shutdownWrite() {
    if (fd_ < 0) return;
    ::shutdown(fd_, SHUT_WR);
}

void EpollTcpConnectionWatcher::onReady(uint32_t events) {
    if (events & EPOLLERR) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
            err = errno;
        }
        fail(err != 0 ? err : EIO);
        return;
    }

    if (events & (EPOLLHUP | EPOLLRDHUP)) {
        peer_closed_ = true;
    }

    if (events & EPOLLIN) handleRead();
    if (events & EPOLLOUT) handleWrite();

    if (peer_closed_ && fd_ >= 0 && out_.empty()) {
        closeNow();
    }
}

void EpollTcpConnectionWatcher::handleRead() {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            in_.assign(buf, buf + n);
            if (on_data_) on_data_(in_);
            continue;
        }
        if (n == 0) {
            peer_closed_ = true;
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        fail(errno);
        return;
    }
}

void EpollTcpConnectionWatcher::handleWrite() {
    while (!out_.empty()) {
        const ssize_t n = ::send(fd_, out_.data(), out_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out_.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            updateEvents(armed_events_ | EPOLLOUT | EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
            return;
        }
        fail(errno);
        return;
    }
    updateEvents((armed_events_ & ~EPOLLOUT) | EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
}

void EpollTcpConnectionWatcher::fail(int err) {
    if (on_error_) on_error_(err);
    closeNow();
}

void EpollTcpConnectionWatcher::closeNow() {
    stop();
    closeFd(fd_);
    if (on_close_) on_close_();
}

PollTcpConnectionWatcher::PollTcpConnectionWatcher(IOWorker& worker,
                                                   int fd,
                                                   OnData on_data,
                                                   OnClose on_close,
                                                   OnError on_error)
    : IWorkerWatcher(worker),
      fd_(fd),
      on_data_(std::move(on_data)),
      on_close_(std::move(on_close)),
      on_error_(std::move(on_error)) {
    createSource(POLLIN | POLLERR | POLLHUP);
}

PollTcpConnectionWatcher::~PollTcpConnectionWatcher() {
    stop();
    source_.reset();
    closeFd(fd_);
}

void PollTcpConnectionWatcher::start() {
    if (attached_) return;
    attachSource(source_);
    attached_ = true;
}

void PollTcpConnectionWatcher::stop() {
    if (!attached_) return;
    detachSource(source_);
    attached_ = false;
}

void PollTcpConnectionWatcher::createSource(uint32_t events) {
    armed_events_ = events;
    source_ = std::make_shared<PollReadinessEventSource>(
        fd_,
        static_cast<short>(events),
        [this](uint32_t ev) { onReady(ev); });
}

void PollTcpConnectionWatcher::updateEvents(uint32_t events) {
    if (!source_) return;
    if (events == armed_events_) return;
    armed_events_ = events;
    source_->setEvents(static_cast<short>(events));
}

void PollTcpConnectionWatcher::send(std::string_view data) {
    if (fd_ < 0) return;
    out_.append(data.data(), data.size());
    updateEvents(armed_events_ | POLLOUT | POLLIN | POLLERR | POLLHUP);
    handleWrite();
}

void PollTcpConnectionWatcher::shutdownWrite() {
    if (fd_ < 0) return;
    ::shutdown(fd_, SHUT_WR);
}

void PollTcpConnectionWatcher::onReady(uint32_t events) {
    if (events & POLLERR) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
            err = errno;
        }
        fail(err != 0 ? err : EIO);
        return;
    }
    if (events & POLLHUP) {
        peer_closed_ = true;
    }
    if (events & POLLIN) handleRead();
    if (events & POLLOUT) handleWrite();

    if (peer_closed_ && fd_ >= 0 && out_.empty()) {
        closeNow();
    }
}

void PollTcpConnectionWatcher::handleRead() {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            in_.assign(buf, buf + n);
            if (on_data_) on_data_(in_);
            continue;
        }
        if (n == 0) {
            peer_closed_ = true;
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        fail(errno);
        return;
    }
}

void PollTcpConnectionWatcher::handleWrite() {
    while (!out_.empty()) {
        const ssize_t n = ::send(fd_, out_.data(), out_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out_.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            updateEvents(armed_events_ | POLLOUT | POLLIN | POLLERR | POLLHUP);
            return;
        }
        fail(errno);
        return;
    }
    updateEvents((armed_events_ & ~POLLOUT) | POLLIN | POLLERR | POLLHUP);
}

void PollTcpConnectionWatcher::fail(int err) {
    if (on_error_) on_error_(err);
    closeNow();
}

void PollTcpConnectionWatcher::closeNow() {
    stop();
    closeFd(fd_);
    if (on_close_) on_close_();
}

} // namespace RopHive::Linux

#endif // __linux__
