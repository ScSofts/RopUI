#include "poll_wakeup.h"

#ifdef __linux__

#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "../poll_backend.h"

namespace RopHive::Linux {

struct PollWakeUpState {
    int fd{-1};

    ~PollWakeUpState() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

PollWakeUpWatcher::PollWakeUpWatcher(EventLoop& loop)
    : IWakeUpWatcher(loop) {

    state_ = std::make_shared<PollWakeUpState>();
    state_->fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (state_->fd < 0) {
        throw std::runtime_error(
            std::string("eventfd failed: ") + std::strerror(errno));
    }

    createSource();
}

PollWakeUpWatcher::~PollWakeUpWatcher() {
    stop();
    // remove from watcher for memory safe

    source_.reset();
    state_.reset();
}

void PollWakeUpWatcher::start() {
    if (attached_) return; // can only attach to one eventloop core

    attachSource(source_);
    attached_ = true;
}

void PollWakeUpWatcher::stop() {
    if (!attached_) return;

    detachSource(source_);
    attached_ = false;
}

void PollWakeUpWatcher::notify() {
    if (!state_ || state_->fd < 0) return;

    uint64_t one = 1;
    ssize_t n = ::write(state_->fd, &one, sizeof(one));
    (void)n; // ignore EAGAIN
}

void PollWakeUpWatcher::createSource() {
    auto state = state_;
    source_ = std::make_shared<PollReadinessEventSource>(
        state ? state->fd : -1,
        POLLIN,
        [state](uint32_t events) {
            if (!(events & POLLIN)) return;

            uint64_t value = 0;
            const int fd = state ? state->fd : -1;
            if (fd < 0) return;
            while (::read(fd, &value, sizeof(value)) > 0) {
                // drain
            }
        });
}

}

#endif // __linux__
