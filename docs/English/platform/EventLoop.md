# EventLoop

## Structure
EventLoop is the real running loop. It essentially ties together IO multiplexing, task queues, and timers:

- `EventLoop`: the public driver layer. It handles `post`/`postDelayed`, timeout calculation, loop scheduling, and exit.
- `EventLoopCore`: the IO syscall proxy layer. It only receives events and dispatches event sources, not task queues.
- `EventCoreBackend`: platform-specific implementation (e.g., Linux epoll/poll).
- `EventSource`: event source abstraction (usually associated with an fd on Linux).
- `WakeUpWatcher`: a wakeup source to pull a blocked IO wait when tasks are posted from other threads.

## Core Interfaces

### EventLoop
Owns the main loop and task scheduling. EventLoop holds a Backend, and all functions are based on several fixed Backend methods:

- `post(Task task)`: post an immediate task, thread-safe.
- `postDelayed(Task task, Duration delay)`: post a delayed task, thread-safe.
- `requestExit()`: request exit, safely wake a blocked loop.
- `attachSource(IEventSource* source)`: register an event source.
- `detachSource(IEventSource* source)`: unregister an event source.
- `run()`: enter the main loop.

### IEventLoopCore
Unified proxy for IO syscalls:

- `runOnce(int timeout = -1)`: execute one IO wait and dispatch.
- `addSource(IEventSource* source)`: add event source (applies later).
- `removeSource(IEventSource* source)`: remove event source (applies later).
- `applyInitialChanges()`: apply pending changes (before start/when needed).

### IEventCoreBackend
Platform-specific backend (e.g. epoll/poll/kqueue):

- `addSource(IEventSource* source)`: register a source to backend.
- `removeSource(IEventSource* source)`: remove a source from backend.
- `wait(int timeout)`: wait for events.
- `rawEvents() const`: return raw event set.

### IEventSource
Event source abstraction, responsible for matching and callbacks:

- `arm(IEventCoreBackend& backend)`: bind to backend.
- `disarm(IEventCoreBackend& backend)`: unbind from backend.
- `matches(const void* raw_event) const`: check if an event belongs to this source.
- `dispatch(const void* raw_event)`: dispatch callback.

### WakeUpWatcher
Triggerable event source, sends a "wake signal" to the loop:

- `notify()`: trigger wakeup, ensure blocked `wait` returns.

## Run Flow
`EventLoop::run()` can be understood as:

1. `applyInitialChanges()`: register initial event sources (including wakeup).
2. `computeTimeoutMs()`: decide wait time based on tasks/timers.
3. `core_->runOnce(timeout)`: wait for IO and dispatch event sources.
4. `runExpiredTimers()`: execute due delayed tasks.
5. `runReadyTasks()`: execute immediate tasks.
6. Loop until `requestExit()`.

## Tasks and Timers

- Immediate tasks are stored in `tasks_` (`deque`).
- Delayed tasks are stored in `timers_` (min-heap by deadline).
- `post()`/`postDelayed()` only enqueue + wake, tasks never run on the calling thread.

`computeTimeoutMs()` rules:

- Has immediate tasks: `timeout = 0` (do not block).
- No timers: `timeout = -1` (wait forever).
- Has timers: pick the closest deadline and compute ms difference.

## Threading and Safety

- `post()`/`postDelayed()` are thread-safe, protected by a mutex.
- `runExpiredTimers()` uses "collect then execute" to reduce lock hold time and avoid deadlock from recursive posting.
- `requestExit()` wakes the loop to ensure exit from blocking.

## Simple Example

```cpp
using namespace RopEventloop;

int main() {
    EventLoop loop(BackendType::LINUX_EPOLL);

    // Register event source (assume MySource inherits IEventSource)
    // MySource source(...);
    // loop.attachSource(&source);

    loop.post([] {
        // Immediate task
    });

    loop.postDelayed([] {
        // Delayed task
    }, std::chrono::milliseconds(16));

    loop.run();
    return 0;
}
```
