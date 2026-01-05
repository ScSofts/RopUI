#include "schedule/worker_watcher.h"

#include "schedule/io_worker.h"

namespace RopHive {

void IWorkerWatcher::attachSource(std::shared_ptr<IEventSource> src) {
    worker_.attachSource(std::move(src));
}

void IWorkerWatcher::detachSource(std::shared_ptr<IEventSource> src) {
    worker_.detachSource(std::move(src));
}

} // namespace RopHive
