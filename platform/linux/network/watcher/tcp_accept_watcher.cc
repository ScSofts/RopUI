#include "tcp_accept_watcher.h"

#ifdef __linux__

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

#include "../../schedule/epoll_backend.h"
#include "../../schedule/poll_backend.h"

namespace RopHive::Linux {

static void acceptLoop(int listen_fd,
                       const std::function<void(int)>& on_accept,
                       const std::function<void(int)>& on_error) {
    for (;;) {
        int client_fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd >= 0) {
            if (on_accept) on_accept(client_fd);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (on_error) on_error(errno);
        return;
    }
}

EpollTcpAcceptWatcher::EpollTcpAcceptWatcher(IOWorker& worker,
                                             int listen_fd,
                                             OnAccept on_accept,
                                             OnError on_error)
    : IWorkerWatcher(worker),
      listen_fd_(listen_fd),
      on_accept_(std::move(on_accept)),
      on_error_(std::move(on_error)) {
    createSource();
}

EpollTcpAcceptWatcher::~EpollTcpAcceptWatcher() {
    stop();
    source_.reset();
}

void EpollTcpAcceptWatcher::start() {
    if (attached_) return;
    attachSource(source_);
    attached_ = true;
}

void EpollTcpAcceptWatcher::stop() {
    if (!attached_) return;
    detachSource(source_);
    attached_ = false;
}

void EpollTcpAcceptWatcher::createSource() {
    source_ = std::make_shared<EpollReadinessEventSource>(
        listen_fd_,
        EPOLLIN | EPOLLERR | EPOLLHUP,
        [this](uint32_t events) { onReady(events); });
}

void EpollTcpAcceptWatcher::onReady(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        if (on_error_) on_error_(EPIPE);
        return;
    }
    if (events & EPOLLIN) {
        acceptLoop(listen_fd_, on_accept_, on_error_);
    }
}

PollTcpAcceptWatcher::PollTcpAcceptWatcher(IOWorker& worker,
                                           int listen_fd,
                                           OnAccept on_accept,
                                           OnError on_error)
    : IWorkerWatcher(worker),
      listen_fd_(listen_fd),
      on_accept_(std::move(on_accept)),
      on_error_(std::move(on_error)) {
    createSource();
}

PollTcpAcceptWatcher::~PollTcpAcceptWatcher() {
    stop();
    source_.reset();
}

void PollTcpAcceptWatcher::start() {
    if (attached_) return;
    attachSource(source_);
    attached_ = true;
}

void PollTcpAcceptWatcher::stop() {
    if (!attached_) return;
    detachSource(source_);
    attached_ = false;
}

void PollTcpAcceptWatcher::createSource() {
    source_ = std::make_shared<PollReadinessEventSource>(
        listen_fd_,
        POLLIN,
        [this](uint32_t events) { onReady(events); });
}

void PollTcpAcceptWatcher::onReady(uint32_t events) {
    if (events & POLLIN) {
        acceptLoop(listen_fd_, on_accept_, on_error_);
    }
}

} // namespace RopHive::Linux

#endif // __linux__

