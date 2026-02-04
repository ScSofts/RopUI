#ifndef _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_CONNECT_WATCHER_H
#define _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_CONNECT_WATCHER_H

#ifdef __linux__

#include <functional>
#include <memory>
#include <string>

#include "../../../schedule/worker_watcher.h"

namespace RopHive::Linux {

// Worker watcher for a non-blocking TCP connect().
// Owns the socket fd until either connected (then transfers ownership to caller)
// or stopped/destroyed (then closes fd).
class EpollTcpConnectWatcher final : public IWorkerWatcher {
public:
    using OnConnected = std::function<void(int connected_fd)>;
    using OnError = std::function<void(int err)>;

    EpollTcpConnectWatcher(IOWorker& worker,
                           std::string host_ipv4,
                           int port,
                           OnConnected on_connected,
                           OnError on_error = {});
    ~EpollTcpConnectWatcher() override;

    void start() override;
    void stop() override;

private:
    void createSocket();
    void startConnect();
    void createSource();
    void onReady(uint32_t events);
    void checkConnectResult();
    void fail(int err);

private:
    std::string host_;
    int port_{0};
    OnConnected on_connected_;
    OnError on_error_;

    int fd_{-1};
    bool attached_{false};
    std::shared_ptr<IEventSource> source_;
};

class PollTcpConnectWatcher final : public IWorkerWatcher {
public:
    using OnConnected = std::function<void(int connected_fd)>;
    using OnError = std::function<void(int err)>;

    PollTcpConnectWatcher(IOWorker& worker,
                          std::string host_ipv4,
                          int port,
                          OnConnected on_connected,
                          OnError on_error = {});
    ~PollTcpConnectWatcher() override;

    void start() override;
    void stop() override;

private:
    void createSocket();
    void startConnect();
    void createSource();
    void onReady(uint32_t events);
    void checkConnectResult();
    void fail(int err);

private:
    std::string host_;
    int port_{0};
    OnConnected on_connected_;
    OnError on_error_;

    int fd_{-1};
    bool attached_{false};
    std::shared_ptr<IEventSource> source_;
};

} // namespace RopHive::Linux

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_CONNECT_WATCHER_H

