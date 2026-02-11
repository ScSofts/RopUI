#ifndef _ROP_PLATFORM_MACOS_NETWORK_WATCHER_TCP_CONNECT_WATCHER_H
#define _ROP_PLATFORM_MACOS_NETWORK_WATCHER_TCP_CONNECT_WATCHER_H

#ifdef __APPLE__

#include <memory>

#include "../../../network/watcher/tcp_watchers.h"
#include "../../../schedule/io_worker.h"

namespace RopHive::MacOS {

std::shared_ptr<RopHive::Network::ITcpConnectWatcher>
createKqueueTcpConnectWatcher(RopHive::IOWorker& worker,
                              RopHive::Network::IpEndpoint remote,
                              RopHive::Network::TcpConnectOption option,
                              RopHive::Network::ITcpConnectWatcher::OnConnected on_connected,
                              RopHive::Network::ITcpConnectWatcher::OnError on_error);

std::shared_ptr<RopHive::Network::ITcpConnectWatcher>
createPollTcpConnectWatcher(RopHive::IOWorker& worker,
                            RopHive::Network::IpEndpoint remote,
                            RopHive::Network::TcpConnectOption option,
                            RopHive::Network::ITcpConnectWatcher::OnConnected on_connected,
                            RopHive::Network::ITcpConnectWatcher::OnError on_error);

} // namespace RopHive::MacOS

#endif // __APPLE__

#endif // _ROP_PLATFORM_MACOS_NETWORK_WATCHER_TCP_CONNECT_WATCHER_H
