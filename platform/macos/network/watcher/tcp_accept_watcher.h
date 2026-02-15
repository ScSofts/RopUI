#ifndef _ROP_PLATFORM_MACOS_NETWORK_WATCHER_TCP_ACCEPT_WATCHER_H
#define _ROP_PLATFORM_MACOS_NETWORK_WATCHER_TCP_ACCEPT_WATCHER_H

#ifdef __APPLE__

#include <memory>

#include "../../../network/watcher/tcp_watchers.h"
#include "../../../schedule/io_worker.h"

namespace RopHive::MacOS {

std::shared_ptr<RopHive::Network::ITcpAcceptWatcher>
createKqueueTcpAcceptWatcher(RopHive::IOWorker& worker,
                             RopHive::Network::TcpAcceptOption option,
                             RopHive::Network::ITcpAcceptWatcher::OnAccept on_accept,
                             RopHive::Network::ITcpAcceptWatcher::OnError on_error);

std::shared_ptr<RopHive::Network::ITcpAcceptWatcher>
createPollTcpAcceptWatcher(RopHive::IOWorker& worker,
                           RopHive::Network::TcpAcceptOption option,
                           RopHive::Network::ITcpAcceptWatcher::OnAccept on_accept,
                           RopHive::Network::ITcpAcceptWatcher::OnError on_error);

} // namespace RopHive::MacOS

#endif // __APPLE__

#endif // _ROP_PLATFORM_MACOS_NETWORK_WATCHER_TCP_ACCEPT_WATCHER_H
