#include "iocp_wakeup.h"

#ifdef _WIN32

#include "../iocp_backend.h"

namespace RopHive::Windows {

static constexpr ULONG_PTR kWakeKey = 0xC0DEC0DE; // unused key, can ignore

IocpWakeUpWatcher::IocpWakeUpWatcher(EventLoop& loop)
    : IWakeUpWatcher(loop) {
    source_ = std::make_shared<IocpWakeUpEventSource>(kWakeKey);
}

IocpWakeUpWatcher::~IocpWakeUpWatcher() {
    stop();
}

void IocpWakeUpWatcher::start() {
    if (attached_) return;
    attachSource(source_);
    attached_ = true;
}

void IocpWakeUpWatcher::stop() {
    if (!attached_) return;
    detachSource(source_);
    attached_ = false;
}

void IocpWakeUpWatcher::notify() {
    if (!attached_) return;
    static_cast<IocpWakeUpEventSource*>(source_.get())->notify();
}

} // namespace RopHive::Windows

#endif // _WIN32

