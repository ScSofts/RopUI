#include "epoll_wakeup.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "../epoll_backend.h"

namespace RopEventloop::Linux {


EpollWakeUpWatcher::EpollWakeUpWatcher(EventLoop& loop)
    : IWakeUpWatcher(loop) {

    wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0) {
        throw std::runtime_error(
            std::string("eventfd failed: ") + std::strerror(errno));
    }

    createSource();
}

EpollWakeUpWatcher::~EpollWakeUpWatcher() {
    stop();

    if (wakeup_fd_ >= 0) {
        ::close(wakeup_fd_);
        wakeup_fd_ = -1;
    }
}

void EpollWakeUpWatcher::start() {
    if (attached_) return;

    attachSource(std::move(source_));
    attached_ = true;
}

void EpollWakeUpWatcher::stop() {
    if (!attached_) return;

    detachSource(source_raw_);
    attached_ = false;
}

void EpollWakeUpWatcher::notify() {
    if (wakeup_fd_ < 0) return;

    uint64_t one = 1;
    ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
    (void)n; // ignore EAGAIN
}

void EpollWakeUpWatcher::createSource() {
    auto src = std::make_unique<EpollReadinessEventSource>(
        wakeup_fd_,
        EPOLLIN,
        [this](uint32_t events) {
            if (!(events & EPOLLIN)) return;

            uint64_t value;
            while (::read(wakeup_fd_, &value, sizeof(value)) > 0) {
                // drain
            }
        });

    source_raw_ = src.get();
    source_ = std::move(src);
}

}
