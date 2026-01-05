#ifndef _ROP_PLATFORM_LINUX_EPOLL_WORKER_WAKEUP_H
#define _ROP_PLATFORM_LINUX_EPOLL_WORKER_WAKEUP_H

#ifdef __linux__

#include <memory>

#include "../../../schedule/worker_watcher.h"

namespace RopHive::Linux {

struct EpollWorkerWakeUpState;

class EpollWorkerWakeUpWatcher final : public IWorkerWakeUpWatcher {
public:
    explicit EpollWorkerWakeUpWatcher(IOWorker& worker);
    ~EpollWorkerWakeUpWatcher() override;

    void start() override;
    void stop() override;
    void notify() override;

private:
    void createSource();

private:
    std::shared_ptr<EpollWorkerWakeUpState> state_;
    bool attached_{false};
    std::shared_ptr<IEventSource> source_;
};

} // namespace RopHive::Linux

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_EPOLL_WORKER_WAKEUP_H
