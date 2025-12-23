#ifndef _ROP_PLATFORM_LINUX_EPOLL_WATCHER_EPOLL_WAKEUP_H
#define _ROP_PLATFORM_LINUX_EPOLL_WATCHER_EPOLL_WAKEUP_H

#include <memory>
#include "../../../schedule/eventloop.h"

namespace RopEventloop::Linux {

class EpollWakeUpWatcher final : public IWakeUpWatcher {
public:
    explicit EpollWakeUpWatcher(EventLoop& loop);
    ~EpollWakeUpWatcher() override;

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

#endif // _ROP_PLATFORM_LINUX_EPOLL_WATCHER_EPOLL_WAKEUP_H