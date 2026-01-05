#ifndef _ROP_PLATFORM_LINUX_EPOLL_WATCHER_EPOLL_WAKEUP_H
#define _ROP_PLATFORM_LINUX_EPOLL_WATCHER_EPOLL_WAKEUP_H

#ifdef __linux__

#include <memory>
#include "../../../schedule/eventloop.h"

namespace RopHive::Linux {

struct EpollWakeUpState;

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
    std::shared_ptr<EpollWakeUpState> state_;

    bool attached_{false};

    std::shared_ptr<IEventSource> source_;
};


}

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_EPOLL_WATCHER_EPOLL_WAKEUP_H
