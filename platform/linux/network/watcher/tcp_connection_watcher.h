#ifndef _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_CONNECTION_WATCHER_H
#define _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_CONNECTION_WATCHER_H

#ifdef __linux__

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "../../../schedule/worker_watcher.h"

namespace RopHive::Linux {

class EpollReadinessEventSource;
class PollReadinessEventSource;

// Worker watcher for a connected non-blocking TCP socket.
// Owns the socket fd and closes it on stop/destruction.
class EpollTcpConnectionWatcher final : public IWorkerWatcher {
public:
    using OnData = std::function<void(std::string_view chunk)>;
    using OnClose = std::function<void()>;
    using OnError = std::function<void(int err)>;

    EpollTcpConnectionWatcher(IOWorker& worker, int fd, OnData on_data, OnClose on_close = {}, OnError on_error = {});
    ~EpollTcpConnectionWatcher() override;

    void start() override;
    void stop() override;

    // Must be called on the worker thread that owns this watcher.
    void send(std::string_view data);
    void shutdownWrite();

private:
    void createSource(uint32_t events);
    void updateEvents(uint32_t events);
    void onReady(uint32_t events);
    void handleRead();
    void handleWrite();
    void fail(int err);
    void closeNow();

private:
    int fd_{-1};
    OnData on_data_;
    OnClose on_close_;
    OnError on_error_;

    bool attached_{false};
    uint32_t armed_events_{0};
    std::shared_ptr<EpollReadinessEventSource> source_;

    std::string out_;
    std::string in_;
    bool peer_closed_{false};
};

class PollTcpConnectionWatcher final : public IWorkerWatcher {
public:
    using OnData = std::function<void(std::string_view chunk)>;
    using OnClose = std::function<void()>;
    using OnError = std::function<void(int err)>;

    PollTcpConnectionWatcher(IOWorker& worker, int fd, OnData on_data, OnClose on_close = {}, OnError on_error = {});
    ~PollTcpConnectionWatcher() override;

    void start() override;
    void stop() override;

    void send(std::string_view data);
    void shutdownWrite();

private:
    void createSource(uint32_t events);
    void updateEvents(uint32_t events);
    void onReady(uint32_t events);
    void handleRead();
    void handleWrite();
    void fail(int err);
    void closeNow();

private:
    int fd_{-1};
    OnData on_data_;
    OnClose on_close_;
    OnError on_error_;

    bool attached_{false};
    uint32_t armed_events_{0};
    std::shared_ptr<PollReadinessEventSource> source_;

    std::string out_;
    std::string in_;
    bool peer_closed_{false};
};

} // namespace RopHive::Linux

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_CONNECTION_WATCHER_H
