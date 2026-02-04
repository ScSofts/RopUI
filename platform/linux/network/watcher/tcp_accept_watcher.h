#ifndef _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_ACCEPT_WATCHER_H
#define _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_ACCEPT_WATCHER_H

#ifdef __linux__

#include <functional>
#include <memory>

#include "../../../schedule/worker_watcher.h"

namespace RopHive::Linux {

// Worker watcher for a listening TCP socket (accept loop on readiness).
// Owns no fd: the caller owns `listen_fd` lifetime.
class EpollTcpAcceptWatcher final : public IWorkerWatcher {
public:
    using OnAccept = std::function<void(int client_fd)>;
    using OnError = std::function<void(int err)>;

    EpollTcpAcceptWatcher(IOWorker& worker, int listen_fd, OnAccept on_accept, OnError on_error = {});
    ~EpollTcpAcceptWatcher() override;

    void start() override;
    void stop() override;

private:
    void createSource();
    void onReady(uint32_t events);

private:
    int listen_fd_{-1};
    OnAccept on_accept_;
    OnError on_error_;

    bool attached_{false};
    std::shared_ptr<IEventSource> source_;
};

class PollTcpAcceptWatcher final : public IWorkerWatcher {
public:
    using OnAccept = std::function<void(int client_fd)>;
    using OnError = std::function<void(int err)>;

    PollTcpAcceptWatcher(IOWorker& worker, int listen_fd, OnAccept on_accept, OnError on_error = {});
    ~PollTcpAcceptWatcher() override;

    void start() override;
    void stop() override;

private:
    void createSource();
    void onReady(uint32_t events);

private:
    int listen_fd_{-1};
    OnAccept on_accept_;
    OnError on_error_;

    bool attached_{false};
    std::shared_ptr<IEventSource> source_;
};

} // namespace RopHive::Linux

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_ACCEPT_WATCHER_H

