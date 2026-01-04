#include "poll_wakeup.h"

#ifdef __linux__

#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "../poll_backend.h"

namespace RopHive::Linux {


PollWakeUpWatcher::PollWakeUpWatcher(EventLoop& loop)
    : IWakeUpWatcher(loop) {

    wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0) {
        throw std::runtime_error(
            std::string("eventfd failed: ") + std::strerror(errno));
    }

    createSource();
}

PollWakeUpWatcher::~PollWakeUpWatcher() {
    stop();
    // remove from watcher for memory safe

    if (wakeup_fd_ >= 0) {
        ::close(wakeup_fd_);
        wakeup_fd_ = -1;
    }
}

void PollWakeUpWatcher::start() {
    if (attached_) return; // can only attach to one eventloop core

    attachSource(source_.get());
    attached_ = true;
}

void PollWakeUpWatcher::stop() {
    if (!attached_) return;

    detachSource(source_.get());
    attached_ = false;
}

void PollWakeUpWatcher::notify() {
    if (wakeup_fd_ < 0) return;

    uint64_t one = 1;
    ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
    (void)n; // ignore EAGAIN
}

void PollWakeUpWatcher::createSource() {
    source_ = std::make_unique<PollReadinessEventSource>(
        wakeup_fd_,
        POLLIN,
        [this](uint32_t events) {
            if (!(events & POLLIN)) return;

            uint64_t value = 0;
            while (::read(wakeup_fd_, &value, sizeof(value)) > 0) {
                // drain
            }
        });
}

}

#endif