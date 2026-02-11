#ifndef _ROP_PLATFORM_MACOS_NETWORK_WATCHER_TCP_CONNECTION_WATCHER_H
#define _ROP_PLATFORM_MACOS_NETWORK_WATCHER_TCP_CONNECTION_WATCHER_H

#ifdef __APPLE__

#include <memory>

#include "../../../network/watcher/tcp_watchers.h"
#include "../../../schedule/io_worker.h"

namespace RopHive::MacOS {

std::shared_ptr<RopHive::Network::ITcpConnectionWatcher>
createKqueueTcpConnectionWatcher(RopHive::IOWorker& worker,
                                 RopHive::Network::TcpConnectionOption option,
                                 std::unique_ptr<RopHive::Network::ITcpStream> connected_stream,
                                 RopHive::Network::ITcpConnectionWatcher::OnRecv on_recv,
                                 RopHive::Network::ITcpConnectionWatcher::OnClose on_close,
                                 RopHive::Network::ITcpConnectionWatcher::OnError on_error,
                                 RopHive::Network::ITcpConnectionWatcher::OnSendReady on_send_ready);

std::shared_ptr<RopHive::Network::ITcpConnectionWatcher>
createPollTcpConnectionWatcher(RopHive::IOWorker& worker,
                               RopHive::Network::TcpConnectionOption option,
                               std::unique_ptr<RopHive::Network::ITcpStream> connected_stream,
                               RopHive::Network::ITcpConnectionWatcher::OnRecv on_recv,
                               RopHive::Network::ITcpConnectionWatcher::OnClose on_close,
                               RopHive::Network::ITcpConnectionWatcher::OnError on_error,
                               RopHive::Network::ITcpConnectionWatcher::OnSendReady on_send_ready);

} // namespace RopHive::MacOS

#endif // __APPLE__

#endif // _ROP_PLATFORM_MACOS_NETWORK_WATCHER_TCP_CONNECTION_WATCHER_H
