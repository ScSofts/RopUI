#ifndef _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_ACCEPT_WATCHER_H
#define _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_ACCEPT_WATCHER_H

#ifdef __linux__

#include <memory>

#include "../../../network/watcher/tcp_watchers.h"
#include "../../../schedule/io_worker.h"

namespace RopHive::Linux {

std::shared_ptr<RopHive::Network::ITcpAcceptWatcher>
createEpollTcpAcceptWatcher(RopHive::IOWorker& worker,
                            RopHive::Network::TcpAcceptOption option,
                            RopHive::Network::ITcpAcceptWatcher::OnAccept on_accept,
                            RopHive::Network::ITcpAcceptWatcher::OnError on_error);

std::shared_ptr<RopHive::Network::ITcpAcceptWatcher>
createPollTcpAcceptWatcher(RopHive::IOWorker& worker,
                           RopHive::Network::TcpAcceptOption option,
                           RopHive::Network::ITcpAcceptWatcher::OnAccept on_accept,
                           RopHive::Network::ITcpAcceptWatcher::OnError on_error);

} // namespace RopHive::Linux

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_ACCEPT_WATCHER_H
