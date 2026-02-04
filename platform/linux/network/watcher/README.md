# Linux TCP worker watchers

This folder contains **IOWorker watchers** (not `EventLoop` watchers) that attach `IEventSource` instances to an `IOWorker`.

## What you get

- `EpollTcpAcceptWatcher` / `PollTcpAcceptWatcher`: watches a non-blocking listen socket and accepts clients on readiness.
- `EpollTcpConnectionWatcher` / `PollTcpConnectionWatcher`: watches a connected non-blocking socket and provides a small `send()` buffer.

## How to use

### 1) Create a listen socket (non-blocking)

Create and configure your `listen_fd` (bind/listen + `O_NONBLOCK`). Then create the accept watcher on the worker thread:

```cpp
auto accept_watcher = std::make_unique<RopHive::Linux::EpollTcpAcceptWatcher>(
    *worker,
    listen_fd,
    [&](int client_fd) {
        // client_fd is already NONBLOCK|CLOEXEC (accept4).
        auto conn = std::make_shared<RopHive::Linux::EpollTcpConnectionWatcher>(
            *worker,
            client_fd,
            [&](std::string_view data) {
                // echo
                conn->send(data);
            },
            [&]() {
                // closed
            });
        conn->start();
    });

accept_watcher->start();
```

### 2) Threading rule

`send()` and `start()/stop()` should be called on the **same worker thread** that owns the watcher.
If you want to write from another thread, post a task to that worker and call `send()` there.

