#ifndef _ROP_PLATFORM_LINUX_EPOLL_WATCHER_POLL_WAKEUP_H
#define _ROP_PLATFORM_LINUX_EPOLL_WATCHER_POLL_WAKEUP_H

#ifdef __linux__

#include <memory>
#include "../../../schedule/eventloop.h"

namespace RopHive::Linux {

struct PollWakeUpState;

class PollWakeUpWatcher final : public IWakeUpWatcher {
public:
    explicit PollWakeUpWatcher(EventLoop& loop);
    ~PollWakeUpWatcher() override;

    void start() override;
    void stop() override;
    void notify() override;

private:
    void createSource();

private:
    std::shared_ptr<PollWakeUpState> state_;

    bool attached_{false};

    std::shared_ptr<IEventSource> source_;
};


}

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_EPOLL_WATCHER_POLL_WAKEUP_H
