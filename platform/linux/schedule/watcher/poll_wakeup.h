#ifndef _ROP_PLATFORM_LINUX_EPOLL_WATCHER_POLL_WAKEUP_H
#define _ROP_PLATFORM_LINUX_EPOLL_WATCHER_POLL_WAKEUP_H

#ifdef __linux__

#include <memory>
#include "../../../schedule/eventloop.h"

namespace RopHive::Linux {

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
    int wakeup_fd_{-1};

    bool attached_{false};

    std::unique_ptr<IEventSource> source_;
};


}

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_EPOLL_WATCHER_POLL_WAKEUP_H