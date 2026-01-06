#ifndef _ROP_PLATFORM_MACOS_EPOLL_WATCHER_POLL_WAKEUP_H
#define _ROP_PLATFORM_MACOS_EPOLL_WATCHER_POLL_WAKEUP_H

#include <memory>
#include <unistd.h>
#include "../../../schedule/worker_watcher.h"

namespace RopHive::MacOS {

struct PollWorkerWakeUpState;

class PollWorkerWakeUpWatcher final : public IWorkerWakeUpWatcher {
public:
    explicit PollWorkerWakeUpWatcher(IOWorker& worker);
    ~PollWorkerWakeUpWatcher() override;

    void start() override;
    void stop() override;
    void notify() override;

private:
    void createSource();

private:
    std::shared_ptr<PollWorkerWakeUpState> state_;
    bool attached_{false};
    std::shared_ptr<IEventSource> source_;
};

}

#endif // _ROP_PLATFORM_MACOS_EPOLL_WATCHER_POLL_WAKEUP_H