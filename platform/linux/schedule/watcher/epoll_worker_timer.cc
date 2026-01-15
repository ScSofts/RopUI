#include "epoll_worker_timer.h"

#ifdef __linux__

#include <sys/timerfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "../epoll_backend.h"

namespace RopHive::Linux {

struct EpollWorkerTimerState {
    int fd{-1};
    std::function<void()> cb{nullptr};

    ~EpollWorkerTimerState() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

EpollWorkerTimerWatcher::EpollWorkerTimerWatcher(IOWorker& worker, std::function<void()> callback)
    : IWorkerWatcher(worker) {
    state_ = std::make_shared<EpollWorkerTimerState>();
    state_->fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (state_->fd < 0) {
        throw std::runtime_error(
            std::string("timerfd_create failed: ") + std::strerror(errno));
    }
    state_->cb = std::move(callback);
    createSource();
}

EpollWorkerTimerWatcher::~EpollWorkerTimerWatcher() {
    stop();
    source_.reset();
    state_.reset();
}

void EpollWorkerTimerWatcher::start() {
    if (attached_) return;
    attachSource(source_);
    attached_ = true;
}

void EpollWorkerTimerWatcher::stop() {
    if (!attached_) return;
    detachSource(source_);
    attached_ = false;
}

void EpollWorkerTimerWatcher::setItimerspec(itimerspec spec) {
    const int fd = state_ ? state_->fd : -1;
    if (fd < 0) return;
    ::timerfd_settime(fd, 0, &spec, nullptr);
}

void EpollWorkerTimerWatcher::clearItimerspec() {
    const int fd = state_ ? state_->fd : -1;
    if (fd < 0) return;
    itimerspec m_spec{};
    ::timerfd_settime(fd, 0, &m_spec, nullptr);
}

void EpollWorkerTimerWatcher::createSource() {
    auto state = state_;
    source_ = std::make_shared<EpollReadinessEventSource>(
        state ? state->fd : -1,
        EPOLLIN,
        [state](uint32_t events) {
            if (!(events & EPOLLIN)) return;
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
                break; // EAGAIN or EOF/error: fd drained for now.
            }
        });
}

} // namespace RopHive::Linux

#endif // __linux__
