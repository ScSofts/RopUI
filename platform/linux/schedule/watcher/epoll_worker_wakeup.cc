#include "epoll_worker_wakeup.h"

#ifdef __linux__

#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "../epoll_backend.h"

namespace RopHive::Linux {

struct EpollWorkerWakeUpState {
    int fd{-1};

    ~EpollWorkerWakeUpState() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

EpollWorkerWakeUpWatcher::EpollWorkerWakeUpWatcher(IOWorker& worker)
    : IWorkerWakeUpWatcher(worker) {
    state_ = std::make_shared<EpollWorkerWakeUpState>();
    state_->fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (state_->fd < 0) {
        throw std::runtime_error(
            std::string("eventfd failed: ") + std::strerror(errno));
    }
    createSource();
}

EpollWorkerWakeUpWatcher::~EpollWorkerWakeUpWatcher() {
    stop();
    source_.reset();
    state_.reset();
}

void EpollWorkerWakeUpWatcher::start() {
    if (attached_) return;
    attachSource(source_);
    attached_ = true;
}

void EpollWorkerWakeUpWatcher::stop() {
    if (!attached_) return;
    detachSource(source_);
    attached_ = false;
}

void EpollWorkerWakeUpWatcher::notify() {
    if (!state_ || state_->fd < 0) return;
    uint64_t one = 1;
    const ssize_t n = ::write(state_->fd, &one, sizeof(one));
    (void)n;
}

void EpollWorkerWakeUpWatcher::createSource() {
    auto state = state_;
    source_ = std::make_shared<EpollReadinessEventSource>(
        state ? state->fd : -1,
        EPOLLIN,
        [state](uint32_t events) {
            if (!(events & EPOLLIN)) return;
            uint64_t value = 0;
            const int fd = state ? state->fd : -1;
            if (fd < 0) return;
            while (::read(fd, &value, sizeof(value)) > 0) {
            }
        });
}

} // namespace RopHive::Linux

#endif // __linux__
