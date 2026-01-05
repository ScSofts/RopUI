#include "epoll_wakeup.h"

#ifdef __linux__

#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "../epoll_backend.h"

namespace RopHive::Linux {

struct EpollWakeUpState {
    int fd{-1};

    ~EpollWakeUpState() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

EpollWakeUpWatcher::EpollWakeUpWatcher(EventLoop& loop)
    : IWakeUpWatcher(loop) {

    state_ = std::make_shared<EpollWakeUpState>();
    state_->fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (state_->fd < 0) {
        throw std::runtime_error(
            std::string("eventfd failed: ") + std::strerror(errno));
    }

    createSource();
}

EpollWakeUpWatcher::~EpollWakeUpWatcher() {
    stop();
    // remove from watcher for memory safe
    source_.reset();
    state_.reset();
}

void EpollWakeUpWatcher::start() {
    if (attached_) return; // can only attach to one eventloop core

    attachSource(source_);
    attached_ = true;
}

void EpollWakeUpWatcher::stop() {
    if (!attached_) return;

    detachSource(source_);
    attached_ = false;
}

void EpollWakeUpWatcher::notify() {
    if (!state_ || state_->fd < 0) return;

    uint64_t one = 1;
    ssize_t n = ::write(state_->fd, &one, sizeof(one));
    (void)n; // ignore EAGAIN
}

void EpollWakeUpWatcher::createSource() {
    auto state = state_;
    source_ = std::make_shared<EpollReadinessEventSource>(
        state ? state->fd : -1,
        EPOLLIN,
        [state](uint32_t events) {
            if (!(events & EPOLLIN)) return;

            uint64_t value;
            const int fd = state ? state->fd : -1;
            if (fd < 0) return;
            while (::read(fd, &value, sizeof(value)) > 0) {
                // drain
            }
        });
}

}

#endif // __linux__
