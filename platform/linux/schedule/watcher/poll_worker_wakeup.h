#ifndef _ROP_PLATFORM_LINUX_POLL_WORKER_WAKEUP_H
#define _ROP_PLATFORM_LINUX_POLL_WORKER_WAKEUP_H

#ifdef __linux__

#include <memory>

#include "../../../schedule/worker_watcher.h"

namespace RopHive::Linux {

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

} // namespace RopHive::Linux

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_POLL_WORKER_WAKEUP_H
