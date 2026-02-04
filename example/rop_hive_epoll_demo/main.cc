#include "platform/linux/schedule/watcher/epoll_worker_timer.h"
#include "platform/schedule/hive.h"
#include "platform/schedule/io_worker.h"
#include "platform/schedule/worker_watcher.h"

#include <iostream>
#include <memory>

static uint64_t test_count = 0;

int main() {
    logger::setMinLevel(LogLevel::DEBUG);
    RopHive::Hive hive;
    auto worker = std::make_shared<RopHive::IOWorker>(hive.options());
    hive.attachIOWorker(worker);

    hive.postIO([]() {
        auto* self = RopHive::IOWorker::currentWorker();
        if (!self) return;

        auto watcher_wp_box =
            std::make_shared<std::weak_ptr<RopHive::Linux::EpollWorkerTimerWatcher>>();
        auto cb = [watcher_wp_box]() {
            std::cout << "postIO(timerfd) " << ++test_count << std::endl;
            if (test_count < 5) return;
            auto watcher = watcher_wp_box ? watcher_wp_box->lock() : nullptr;
            if (!watcher) return;
            watcher->clearItimerspec();
            watcher->stop();

            if (auto* w = RopHive::IOWorker::currentWorker()) {
                w->releaseWatcher(watcher.get());
            }
        };

        auto watcher = std::make_shared<RopHive::Linux::EpollWorkerTimerWatcher>(*self, std::move(cb));
        *watcher_wp_box = watcher;
        self->adoptWatcher(watcher);

        watcher->setItimerspec(itimerspec{{1, 0}, {2, 0}});
        watcher->start();
    });

    hive.run();
    return 0;
}
