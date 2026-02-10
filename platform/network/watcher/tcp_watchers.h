#ifndef _ROP_PLATFORM_NETWORK_WATCHER_TCP_WATCHERS_H
#define _ROP_PLATFORM_NETWORK_WATCHER_TCP_WATCHERS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "../../schedule/worker_watcher.h"
#include "../ip_endpoint.h"

namespace RopHive {
class IOWorker;
}

namespace RopHive::Network {

// ---- Connected stream envelope ----

enum class TcpStreamKind : uint8_t {
  // Linux
  LinuxEpoll,
  LinuxPoll,

  // macOS
  MacKqueue,
  MacPoll,

  // Windows
  WindowsIocp,
};

// A connected TCP stream "envelope" that can carry backend/platform specific
// data.
struct ITcpStream {
  explicit ITcpStream(TcpStreamKind k) : kind(k) {}
  virtual ~ITcpStream() = default;

  TcpStreamKind kind;

  // Best-effort metadata for upper layers (logging/sharding).
  // `std::nullopt` means "unknown/unset".
  std::optional<IpEndpoint> peer;
  std::optional<IpEndpoint> local;
};

// ---- Options ----

struct TcpAcceptOption {
  // Local bind endpoint (direct IP, no DNS). Default is 0.0.0.0:0.
  // Note: port=0 means ephemeral (not typical for listening, but allowed by
  // OS).
  IpEndpoint local = IpEndpointV4{};

  // Listen phase
  int backlog = 128;

    // Accept phase
  size_t max_accept_per_tick = 64;
  bool fill_endpoints = true;

  // Optional (best-effort) listen socket options: std::nullopt means "do not
  // touch / not allowed to take effect".
  std::optional<bool> reuse_addr;
  std::optional<bool> reuse_port; // may be ignored on some platforms
  std::optional<bool> ipv6_only;  // only meaningful for IPv6 listen sockets

  // Optional (best-effort) socket options: std::nullopt means "do not touch /
  // not allowed to take effect".
  std::optional<bool> set_close_on_exec;
  std::optional<bool> tcp_no_delay;
  std::optional<bool> keep_alive;
  std::optional<int> keep_alive_idle_sec;
  std::optional<int> keep_alive_interval_sec;
  std::optional<int> keep_alive_count;
  std::optional<int> recv_buf_bytes;
  std::optional<int> send_buf_bytes;
};

struct TcpConnectOption {
  std::optional<IpEndpoint> local_bind;
  uint16_t local_port = 0; // 0 means ephemeral; ignored if local_bind is set

  bool fill_endpoints = true;
  size_t connected_queue_capacity = 1024;
  // reserved for future "queue+drain" model

  // Optional (best-effort) socket options: std::nullopt means "do not touch /
  // not allowed to take effect".
  std::optional<bool> set_close_on_exec;
  std::optional<bool> tcp_no_delay;
  std::optional<bool> keep_alive;
  std::optional<int> keep_alive_idle_sec;
  std::optional<int> keep_alive_interval_sec;
  std::optional<int> keep_alive_count;
  std::optional<int> recv_buf_bytes;
  std::optional<int> send_buf_bytes;
};

struct TcpConnectionOption {
  size_t max_read_bytes_per_tick = 256 * 1024;
  size_t max_write_bytes_per_tick = 256 * 1024;
  // used only for fairness; trySend may still be called by user

  // Optional (best-effort) socket options: std::nullopt means "do not touch /
  // not allowed to take effect".
  std::optional<bool> tcp_no_delay;
  std::optional<bool> keep_alive;
  std::optional<int> keep_alive_idle_sec;
  std::optional<int> keep_alive_interval_sec;
  std::optional<int> keep_alive_count;
  std::optional<int> recv_buf_bytes;
  std::optional<int> send_buf_bytes;
  std::optional<int> linger_sec;
};

// ---- Watcher interfaces ----

class ITcpAcceptWatcher : public IWorkerWatcher {
public:
  using OnAccept = std::function<void(std::unique_ptr<ITcpStream> stream)>;
  using OnError = std::function<void(int err)>;

  using IWorkerWatcher::IWorkerWatcher;
  ~ITcpAcceptWatcher() override = default;
};

class ITcpConnectWatcher : public IWorkerWatcher {
public:
  using OnConnected = std::function<void(std::unique_ptr<ITcpStream> stream)>;
  using OnError = std::function<void(int err)>;

  using IWorkerWatcher::IWorkerWatcher;
  ~ITcpConnectWatcher() override = default;

  virtual void cancel() = 0;
};

class ITcpConnectionWatcher : public IWorkerWatcher {
public:
  using OnRecv = std::function<void(std::string_view chunk)>;
  using OnClose = std::function<void()>;
  using OnError = std::function<void(int err)>;
  using OnSendReady = std::function<void()>;

  struct TrySendResult {
    size_t n = 0;
    bool would_block = false;
    int err = 0;
  };

  using IWorkerWatcher::IWorkerWatcher;
  ~ITcpConnectionWatcher() override = default;

  virtual TrySendResult trySend(std::string_view data) = 0;
  virtual void shutdownWrite() = 0;
  virtual void close() = 0;
};

// ---- Factory API ----

std::shared_ptr<ITcpAcceptWatcher>
createTcpAcceptWatcher(IOWorker &worker, TcpAcceptOption option,
                       ITcpAcceptWatcher::OnAccept on_accept,
                       ITcpAcceptWatcher::OnError on_error = {});

std::shared_ptr<ITcpConnectWatcher>
createTcpConnectWatcher(IOWorker &worker, IpEndpoint remote,
                        TcpConnectOption option,
                        ITcpConnectWatcher::OnConnected on_connected,
                        ITcpConnectWatcher::OnError on_error = {});

std::shared_ptr<ITcpConnectionWatcher> createTcpConnectionWatcher(
    IOWorker &worker, TcpConnectionOption option,
    std::unique_ptr<ITcpStream> connected_stream,
    ITcpConnectionWatcher::OnRecv on_recv,
    ITcpConnectionWatcher::OnClose on_close = {},
    ITcpConnectionWatcher::OnError on_error = {},
    ITcpConnectionWatcher::OnSendReady on_send_ready = {});

} // namespace RopHive::Network

#endif // _ROP_PLATFORM_NETWORK_WATCHER_TCP_WATCHERS_H
