#include <algorithm>

#include <log.hpp>
#include "eventloop_core.h"

namespace RopHive {

IEventLoopCore::IEventLoopCore(std::unique_ptr<IEventCoreBackend> backend)
    : backend_(std::move(backend)) {}

void IEventLoopCore::addSource(std::shared_ptr<IEventSource> source) {
    if (!source) return;
    LOG(DEBUG)("source add to core");
    std::lock_guard<std::mutex> lock(mu_);
    pending_add_.push_back(std::move(source));
}

void IEventLoopCore::removeSource(std::shared_ptr<IEventSource> source) {
    if (!source) return;
    std::lock_guard<std::mutex> lock(mu_);
    pending_remove_.push_back(std::move(source));
}

void IEventLoopCore::runOnce(int timeout) {
    // LOG(DEBUG)("event loop core iteration started");
    backend_->wait(timeout);
    // LOG(DEBUG)("event loop core after waited");
    dispatchRawEvents();
    applyPendingChanges();
}

void IEventLoopCore::dispatchRawEvents() {
    if (in_dispatch_) {
        return;
    }

    in_dispatch_ = true;

    RawEventSpan span = backend_->rawEvents();
    const char* base = static_cast<const char*>(span.data);
    std::vector<std::shared_ptr<IEventSource>> sources_snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        sources_snapshot = sources_;
    }
    // LOG(DEBUG)("span.count: %d", span.count);
    for (size_t i = 0; i < span.count; ++i) {
        const void* ev = base + i * span.stride;

        for (auto& src : sources_snapshot) {
            if (src && src->matches(ev)) {
                src->dispatch(ev);
                break;
            }
        }
    }

    in_dispatch_ = false;
}

void IEventLoopCore::applyPendingChanges() {
    if (in_dispatch_) {
        return;
    }

    std::vector<std::shared_ptr<IEventSource>> pending_add;
    std::vector<std::shared_ptr<IEventSource>> pending_remove;
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_add.swap(pending_add_);
        pending_remove.swap(pending_remove_);
    }

    for (auto& src : pending_add) {
        if (!src) continue;
        backend_->addSource(src.get());
        src->arm(*backend_);
        
        sources_.push_back(std::move(src));
    }

    for (auto& src : pending_remove) {
        if (!src) continue;
        src->disarm(*backend_);
        backend_->removeSource(src.get());

        auto it = std::remove_if(
            sources_.begin(),
            sources_.end(),
            [&src](const std::shared_ptr<IEventSource>& p) {
                return p && p.get() == src.get();
            });
        sources_.erase(it, sources_.end());
    }
}

}
