#include "tcp_connect_watcher.h"

#ifdef __linux__

#include <arpa/inet.h>
#include <fcntl.h>
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

EpollTcpConnectWatcher::EpollTcpConnectWatcher(IOWorker& worker,
                                               std::string host_ipv4,
                                               int port,
                                               OnConnected on_connected,
                                               OnError on_error)
    : IWorkerWatcher(worker),
      host_(std::move(host_ipv4)),
      port_(port),
      on_connected_(std::move(on_connected)),
      on_error_(std::move(on_error)) {}

EpollTcpConnectWatcher::~EpollTcpConnectWatcher() {
    stop();
    source_.reset();
    closeFd(fd_);
}

void EpollTcpConnectWatcher::start() {
    if (attached_) return;
    createSocket();
    startConnect();
    createSource();
    attachSource(source_);
    attached_ = true;
}

void EpollTcpConnectWatcher::stop() {
    if (!attached_) return;
    detachSource(source_);
    attached_ = false;
}

void EpollTcpConnectWatcher::createSocket() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        fail(errno);
        return;
    }
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
}

void EpollTcpConnectWatcher::startConnect() {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        fail(EINVAL);
        return;
    }

    const int rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        checkConnectResult();
        return;
    }
    if (errno == EINPROGRESS) {
        return;
    }
    fail(errno);
}

void EpollTcpConnectWatcher::createSource() {
    source_ = std::make_shared<EpollReadinessEventSource>(
        fd_,
        EPOLLOUT | EPOLLERR | EPOLLHUP,
        [this](uint32_t events) { onReady(events); });
}

void EpollTcpConnectWatcher::onReady(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        checkConnectResult();
        return;
    }
    if (events & EPOLLOUT) {
        checkConnectResult();
        return;
    }
}

void EpollTcpConnectWatcher::checkConnectResult() {
    if (fd_ < 0) return;
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        fail(errno);
        return;
    }
    if (err != 0) {
        fail(err);
        return;
    }

    const int connected_fd = fd_;
    fd_ = -1;
    stop();
    if (on_connected_) on_connected_(connected_fd);
}

void EpollTcpConnectWatcher::fail(int err) {
    stop();
    if (on_error_) on_error_(err);
}

PollTcpConnectWatcher::PollTcpConnectWatcher(IOWorker& worker,
                                             std::string host_ipv4,
                                             int port,
                                             OnConnected on_connected,
                                             OnError on_error)
    : IWorkerWatcher(worker),
      host_(std::move(host_ipv4)),
      port_(port),
      on_connected_(std::move(on_connected)),
      on_error_(std::move(on_error)) {}

PollTcpConnectWatcher::~PollTcpConnectWatcher() {
    stop();
    source_.reset();
    closeFd(fd_);
}

void PollTcpConnectWatcher::start() {
    if (attached_) return;
    createSocket();
    startConnect();
    createSource();
    attachSource(source_);
    attached_ = true;
}

void PollTcpConnectWatcher::stop() {
    if (!attached_) return;
    detachSource(source_);
    attached_ = false;
}

void PollTcpConnectWatcher::createSocket() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        fail(errno);
        return;
    }
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
}

void PollTcpConnectWatcher::startConnect() {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        fail(EINVAL);
        return;
    }

    const int rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        checkConnectResult();
        return;
    }
    if (errno == EINPROGRESS) {
        return;
    }
    fail(errno);
}

void PollTcpConnectWatcher::createSource() {
    source_ = std::make_shared<PollReadinessEventSource>(
        fd_,
        POLLOUT,
        [this](uint32_t events) { onReady(events); });
}

void PollTcpConnectWatcher::onReady(uint32_t events) {
    if (events & POLLOUT) {
        checkConnectResult();
        return;
    }
}

void PollTcpConnectWatcher::checkConnectResult() {
    if (fd_ < 0) return;
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        fail(errno);
        return;
    }
    if (err != 0) {
        fail(err);
        return;
    }

    const int connected_fd = fd_;
    fd_ = -1;
    stop();
    if (on_connected_) on_connected_(connected_fd);
}

void PollTcpConnectWatcher::fail(int err) {
    stop();
    if (on_error_) on_error_(err);
}

} // namespace RopHive::Linux

#endif // __linux__

