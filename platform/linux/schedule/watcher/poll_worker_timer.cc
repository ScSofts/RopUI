#include "poll_worker_timer.h"

#ifdef __linux__

#include <sys/timerfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "../poll_backend.h"

namespace RopHive::Linux {

struct PollWorkerTimerState {
    int fd{-1};
    std::function<void()> cb{nullptr};

    ~PollWorkerTimerState() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

PollWorkerTimerWatcher::PollWorkerTimerWatcher(IOWorker& worker, std::function<void()> callback)
    : IWorkerWatcher(worker) {
    state_ = std::make_shared<PollWorkerTimerState>();
    state_->fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (state_->fd < 0) {
        throw std::runtime_error(
            std::string("timerfd_create failed: ") + std::strerror(errno));
    }
    state_->cb = std::move(callback);
    createSource();
}

PollWorkerTimerWatcher::~PollWorkerTimerWatcher() {
    stop();
    source_.reset();
    state_.reset();
}

void PollWorkerTimerWatcher::start() {
    if (attached_) return;
    attachSource(source_);
    attached_ = true;
}

void PollWorkerTimerWatcher::stop() {
    if (!attached_) return;
    detachSource(source_);
    attached_ = false;
}

void PollWorkerTimerWatcher::setItimerspec(itimerspec spec) {
    const int fd = state_ ? state_->fd : -1;
    if (fd < 0) return;
    ::timerfd_settime(fd, 0, &spec, nullptr);
}

void PollWorkerTimerWatcher::clearItimerspec() {
    const int fd = state_ ? state_->fd : -1;
    if (fd < 0) return;
    itimerspec m_spec{};
    ::timerfd_settime(fd, 0, &m_spec, nullptr);
}

void PollWorkerTimerWatcher::createSource() {
    auto state = state_;
    source_ = std::make_shared<PollReadinessEventSource>(
        state ? state->fd : -1,
        POLLIN,
        [state](short revents) {
            if (!(revents & POLLIN)) return;
            const int fd = state ? state->fd : -1;
            if (fd < 0) return;
            for (;;) {
                uint64_t expirations = 0;
                const ssize_t n = ::read(fd, &expirations, sizeof(expirations));
                if (n == static_cast<ssize_t>(sizeof(expirations))) {
                    if (state->cb) {
                        for (uint64_t i = 0; i < expirations; ++i) {
                            state->cb();
                        }
                    }
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                break;
            }
        });
}

} // namespace RopHive::Linux

#endif // __linux__
