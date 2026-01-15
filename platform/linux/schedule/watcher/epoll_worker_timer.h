#ifndef _ROP_PLATFORM_LINUX_EPOLL_WORKER_TIMER_H
#define _ROP_PLATFORM_LINUX_EPOLL_WORKER_TIMER_H

#ifdef __linux__

#include <functional>
#include <memory>

#include "../../../schedule/worker_watcher.h"

namespace RopHive::Linux {

struct EpollWorkerTimerState;

// A worker watcher backed by timerfd + epoll; allows setting an arbitrary itimerspec.
// The callback is invoked once per expiration (expirations are drained via read()).
class EpollWorkerTimerWatcher final : public IWorkerWatcher {
public:
    explicit EpollWorkerTimerWatcher(IOWorker& worker, std::function<void()> callback);
    ~EpollWorkerTimerWatcher() override;

    void start() override;
    void stop() override;

    void setItimerspec(itimerspec spec);
    void clearItimerspec();

private:
    void createSource();

private:
    std::shared_ptr<EpollWorkerTimerState> state_;
    bool attached_{false};
    std::shared_ptr<IEventSource> source_;
};

} // namespace RopHive::Linux

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_EPOLL_WORKER_TIMER_H
