# PC 端 TCP 网络 Watcher 统一设计

本文档面向 **Windows / macOS / Linux（PC 三端）**，设计一套统一的 TCP Server/Client Watcher 接口与实现约束，使其可以无缝挂载到现有的 `RopHive::IOWorker`（`IWorkerWatcher` + `IEventSource`）体系中，并能在不同平台后端（epoll/poll/kqueue/IOCP）下得到等价行为。

> 约束：Watcher 只负责“套接字生命周期 + 事件驱动收发/建连/accept”，不内置域名解析、TLS、重连等策略；这些属于更上层组件。

---

## 1. 背景与目标

### 1.1 背景
- 仓库目前有一套 Linux TCP worker watchers：`platform/linux/network/watcher/`（accept/connect/connection）。**它们仅供参考**，后续会按本文档的统一接口进行重构/替换；不要把它们当成最终公共 API 或跨平台架构的依据。
- 调度层提供了统一的 `IEventLoopCore/IEventSource` 抽象，并在不同平台存在不同后端：
  - Linux: epoll/poll（readiness）
  - macOS: kqueue/poll/cocoa（readiness + 系统事件）
  - Windows: IOCP/win32（completion + 系统消息）

### 1.2 目标
1) 提供 **统一接口**（同名、同语义、同线程规则）覆盖 TCP 的三个底层阶段：
   - Listen/Accept（server）
   - Non-blocking Connect（client）
   - Connected Read/Write（both）
2) Watcher 在各平台的实现必须：
   - 可挂载到任意 `IOWorker`（只依赖 `IWorkerWatcher`、`IEventSource` 与 worker 的 backend type）
   - 不阻塞 worker 线程（尤其是域名解析）
   - 对外暴露一致的错误码与关闭语义

### 1.3 非目标
- 不在 watcher 内部做 **域名解析**（DNS）：上层做，或提供独立 resolver 组件。
- 不在 watcher 内部做 **TLS/SSL**：上层用一个 `TlsConnection` 包装 `TcpConnectionWatcher`。
- 不做跨平台的“通用 socket 创建/选项”大一统：仅给出最小的 socket 约束与建议工具函数。

---

## 2. 统一抽象：命名空间与文件布局

建议新增统一入口层（仅接口与工厂）：
- `platform/network/watcher/tcp_watchers.h`：统一接口与工厂函数（**不含平台细节**）
- `platform/<os>/network/watcher/`：平台实现

命名建议（统一层）：
- `namespace RopHive::Network { ... }`

平台实现建议（保持现有风格）：
- `RopHive::Linux::...`
- `RopHive::MacOS::...`
- `RopHive::Windows::...`

---

## 3. 线程与生命周期规则（必须遵守）

### 3.1 线程归属（Owner Worker）
- 每个 watcher 都绑定一个 `IOWorker& worker_`，称为 **owner worker**。
- `start()/stop()`、`send()/shutdownWrite()/close()`（若提供）必须在 **owner worker 线程**调用。
- 跨线程调用的正确方式：`hive.postToWorker(id, ...)` 或 `worker.postPrivate(...)` 让操作回到 owner worker 执行。

### 3.2 资源所有权
- `AcceptWatcher`：默认 **不拥有** `listen socket`（由上层创建并管理其生命周期）。
- `ConnectWatcher`：默认 **拥有**其内部创建的 socket，成功后把连接 socket 的所有权 **转移**给上层/ConnectionWatcher。
- `ConnectionWatcher`：**拥有**连接 socket，`stop()/析构` 时关闭 fd/socket。

### 3.3 回调与捕获约束（与现有 watcher 设计保持一致）
- 回调尽量不要捕获裸 `this`；需要跨回调持有 watcher 生命周期时，用 `shared_ptr` / `weak_ptr` 兜底（参考 `docs/Chinese/platform/跨平台调度器开发.md` 中 watcher 设计建议）。
- Watcher 的 `IEventSource` 通常保存 callback；callback 内部不要假设 watcher 仍存活，除非有明确的生命周期托管机制。

### 3.4 “取消/停止”语义
统一接口应包含“取消连接尝试/关闭连接”的显式方法（见第 4 节）。实现必须保证：
- `stop()` 之后不会再触发任何用户回调（或仅允许一次 `on_close/on_error`，但要定义清楚并统一）。
- `cancel/close` 要尽快释放底层句柄（Windows 上通常要配合 `CancelIoEx`/关闭 socket 使 IO 完成返回）。

---

## 4. 统一接口设计（头文件骨架）

> 这里给出“工作文档级”的接口草案：目的、方法语义、实现要点。实际落地可根据代码风格微调命名，但**语义必须一致**。

### 4.1 统一错误码
统一对外暴露 `int err`：
- POSIX：使用 `errno` 值（如 `ECONNRESET/EPIPE/ETIMEDOUT`）
- Windows：使用 `WSAGetLastError()` 值（如 `WSAECONNRESET/WSAETIMEDOUT`）

上层如需统一字符串：提供 `Network::errorToString(err)`（后续可加）。

### 4.2 地址/端点数据结构
为避免把 DNS 绑死在 watcher 里，connect 的输入建议是“已解析”的 socket 地址：

```cpp
namespace RopHive::Network {

struct TcpEndpoint {
    // 已解析的地址（IPv4/IPv6）。在 POSIX 为 sockaddr_storage；Windows 也可复用。
    sockaddr_storage addr{};
    socklen_t addr_len{0};

    // 可选：用于日志/调试的原始文本（不是必须字段）。
    std::string debug_name;
};

} // namespace RopHive::Network
```

如果阶段 1 只想支持 IPv4 字符串，也可以先提供辅助函数把 `host_ipv4+port` 转成 `TcpEndpoint`，但统一接口仍以 `TcpEndpoint` 为核心。

### 4.3 ITcpAcceptWatcher（Server）
职责：监听 socket readiness → accept 循环 → 为每个新连接触发回调。

```cpp
namespace RopHive::Network {

class ITcpAcceptWatcher : public IWorkerWatcher {
public:
    struct Client {
        // POSIX: fd；Windows: SOCKET (cast to intptr_t) 或单独 typedef。
        intptr_t handle{0};
        sockaddr_storage peer{};
        socklen_t peer_len{0};
    };

    using OnAccept = std::function<void(Client client)>;
    using OnError  = std::function<void(int err)>;

    virtual ~ITcpAcceptWatcher() = default;

    // start(): attach source(s) to worker; begin receiving accept events.
    // stop(): detach source(s); stop future callbacks.
};

// Factory: create an accept watcher for current worker backend.
std::shared_ptr<ITcpAcceptWatcher>
createTcpAcceptWatcher(IOWorker& worker,
                       intptr_t listen_handle,
                       ITcpAcceptWatcher::OnAccept on_accept,
                       ITcpAcceptWatcher::OnError on_error = {});

} // namespace RopHive::Network
```

实现要点：
- 必须使用 non-blocking accept loop（POSIX: `accept4` + `EAGAIN/EINTR`）。
- `on_accept` 触发时，客户端 socket 必须处于非阻塞模式（POSIX 可用 `accept4(..., SOCK_NONBLOCK|SOCK_CLOEXEC)`）。
- 错误处理建议：listen socket readiness 上的 `ERR/HUP` 要尽量用 `getsockopt(SO_ERROR)` 取真实错误码，而不是常量替代。

### 4.4 ITcpConnectWatcher（Client）
职责：发起非阻塞连接，连接成功后把“已连接 socket”移交给上层（通常用来创建 `ITcpConnectionWatcher`）。

```cpp
namespace RopHive::Network {

class ITcpConnectWatcher : public IWorkerWatcher {
public:
    struct Connected {
        intptr_t handle{0};
        sockaddr_storage peer{};
        socklen_t peer_len{0};
    };

    using OnConnected = std::function<void(Connected c)>;
    using OnError     = std::function<void(int err)>;

    virtual ~ITcpConnectWatcher() = default;

    // cancel(): 显式取消连接尝试并释放句柄；允许多次调用。
    // 语义：cancel() 之后不再触发 on_connected；on_error 是否触发需要统一规定：
    // - 推荐：cancel() 不触发 on_error（由上层自行决定是否视为错误）。
    virtual void cancel() = 0;
};

std::shared_ptr<ITcpConnectWatcher>
createTcpConnectWatcher(IOWorker& worker,
                        TcpEndpoint endpoint,
                        ITcpConnectWatcher::OnConnected on_connected,
                        ITcpConnectWatcher::OnError on_error = {});

} // namespace RopHive::Network
```

钩子名称,触发时机,适用范围
onConnect,Client 专用：当异步连接成功建立时。,Client
onAccept,Server 专用：当新客户端被接入并握手成功。,Server
onMessage,收到数据。,两者
onSendReady,缓冲区可写。,两者
onError,连接失败（如超时、拒绝连接）。,两者
onClose,对端关闭或主动断开。,两者

实现要点：
- POSIX：`socket` + `O_NONBLOCK` + `connect`，`EINPROGRESS` 后监听可写，再用 `getsockopt(SO_ERROR)` 判定。
- Windows(IOCP)：使用 `ConnectEx`（或非阻塞 connect + WSAEventSelect 方案不推荐）。`cancel()` 需要能尽快终止 pending connect（通常靠 `closesocket` 触发完成，或 `CancelIoEx`/`CancelIo`）。

### 4.5 ITcpConnectionWatcher（Connected IO）
职责：对一个已连接的 socket，基于 worker 后端驱动读写与关闭事件；提供一个最小的发送缓冲。

```cpp
namespace RopHive::Network {

class ITcpConnectionWatcher : public IWorkerWatcher {
public:
    using OnData  = std::function<void(std::string_view chunk)>;
    using OnClose = std::function<void()>;
    using OnError = std::function<void(int err)>;

    virtual ~ITcpConnectionWatcher() = default;

    // send(): append to internal buffer and arm write readiness / post async send.
    virtual void send(std::string_view data) = 0;

    // shutdownWrite(): half-close write side.
    virtual void shutdownWrite() = 0;

    // close(): actively close immediately (optional but strongly recommended).
    virtual void close() = 0;

    // Optional introspection:
    virtual size_t pendingWriteBytes() const noexcept = 0;
};

std::shared_ptr<ITcpConnectionWatcher>
createTcpConnectionWatcher(IOWorker& worker,
                           intptr_t connected_handle,
                           ITcpConnectionWatcher::OnData on_data,
                           ITcpConnectionWatcher::OnClose on_close = {},
                           ITcpConnectionWatcher::OnError on_error = {});

} // namespace RopHive::Network
```

实现要点：
- POSIX readiness：
  - `EPOLLIN/POLLIN` → `recv` loop until `EAGAIN`；`n==0` 视为 peer close。
  - `EPOLLOUT/POLLOUT` → `send` loop until `EAGAIN`，并在 out buffer 为空时取消写监听。
  - 尽量订阅 `RDHUP/HUP/ERR` 并处理 half-close/close（Linux 可用 `EPOLLRDHUP`）。
- Windows(IOCP) completion：
  - 常用模式是 “永远保持一个 pending recv” + “写队列串行发出 overlapped WSASend”。
  - 需要为每个 I/O op 持有独立的 `OVERLAPPED` 与 buffer；完成回调中再投递下一次 recv/send。
  - close/cancel 必须处理 pending I/O：关闭 socket 通常会让 pending op 以 error completion 回来；实现需屏蔽 stop 后的回调。

---

## 5. 平台实现策略（落地路径）

### 5.1 Linux（epoll/poll）
- 现有实现可作为“语义参考 / 原型验证”（但需要重构以满足统一接口与取消语义等要求）：
  - `platform/linux/network/watcher/tcp_accept_watcher.*`
  - `platform/linux/network/watcher/tcp_connect_watcher.*`
  - `platform/linux/network/watcher/tcp_connection_watcher.*`
- 统一重构时建议：
  - 提供 `Network::createTcp*Watcher(...)` 工厂：根据 `worker.options().io_backend` 或 `worker.core()->backendType()` 选择 epoll/poll 实现。
  - 完善错误码：accept/connect 的错误尽量用 `SO_ERROR` 获取真实值。
  - 明确 cancel 行为：connect watcher `cancel()` 应立即 `close(fd)`（现在 stop 不 close 的行为不利于超时/取消）。

### 5.2 macOS（kqueue/poll）
- 使用 `platform/macos/schedule/kqueue_backend.h` 的 `KqueueReadinessEventSource` 与 `PollReadinessEventSource`。
- POSIX socket API 与 Linux 类似（同样 `recv/send`、`O_NONBLOCK`、`getsockopt(SO_ERROR)`）。
- 注意：
  - kqueue 使用 `(ident=fd, filter=EVFILT_READ/EVFILT_WRITE)` 区分读写；通常需要为同一 fd 注册两类 source（或在一个 source 内动态改 filter/flags）。

### 5.3 Windows（IOCP）
- 使用 `platform/windows/schedule/iocp_backend.h` 的 `IocpHandleCompletionEventSource`：
  - arm() 时把 socket 关联到 IOCP（`associateHandle`）
  - dispatch() 收到完成事件后，分发给 watcher 内部状态机
- 核心 API（建议）：
  - server accept：`AcceptEx`
  - client connect：`ConnectEx`
  - connection recv/send：`WSARecv/WSASend`
- 关键实现要求：
  - 每个连接必须有一份 `ConnectionState`（包含 socket、读写 buffer、overlapped structs、关闭标记）。
  - stop/cancel/close 必须“幂等”，并屏蔽 stop 后的回调（避免 use-after-free）。

---

## 6. 与 Hive/IOWorker 的集成方式（推荐用法）

### 6.1 Server（单 acceptor + 分发连接到 worker）
推荐：一个 worker 专职 accept，accept 到的 client socket 通过 `Hive::postToWorker(...)` 分发到目标 worker，在目标 worker 上创建 `ConnectionWatcher`：

1) accept worker 上：`AcceptWatcher` 回调拿到 `client handle`
2) 选择目标 worker（轮询/哈希/负载）
3) `hive.postToWorker(target, [client] { createTcpConnectionWatcher(...)->start(); })`

这样可以让连接的读写 watcher 与其工作负载自然绑定到某个 worker。

### 6.2 Client（resolve → connect → connection）
- DNS/解析：在 compute 池或独立 resolver 做（避免 IO worker 阻塞）。
- connect watcher 仅接 `TcpEndpoint`（已解析地址）。

---

## 7. 覆盖面清单（底层“必须覆盖”的接口场景）

为了保证“统一 watcher 能覆盖 PC 端 TCP 的底层阶段”，实现必须覆盖：
- listen：accept 循环（`EINTR/EAGAIN`），支持高并发 accept burst
- connect：非阻塞 connect 成功/失败判定（`SO_ERROR` / Windows completion error）
- connection：
  - 读：连续读取直到 `EAGAIN` / completion，正确处理 `n==0` 的对端关闭
  - 写：发送缓冲 + 可写事件/完成驱动；支持部分写与 `EAGAIN`
  - close：对端关闭/本端关闭都要可观测（`on_close` 或 `on_error`，需一致）

建议额外覆盖（但可作为后续迭代）：
- peer/local 地址获取（accept/connect 回调返回 peer 信息）
- 显式 cancel（connect/connection），并在 stop 后不再回调用户

---

## 8. 后续扩展点（不进入 watcher 核心）

保持 watcher “薄而硬”，将以下功能放在上层组件：
- `DnsResolver`：跨平台解析（线程池包装 `getaddrinfo` / Windows `GetAddrInfoEx`）
- `TcpClient`：重连/退避/多地址尝试（Happy Eyeballs）
- `TcpServer`：多 acceptor（Linux `SO_REUSEPORT`）/连接分桶/连接池
- `TlsConnection`：对 `ITcpConnectionWatcher` 进行 TLS 包装
