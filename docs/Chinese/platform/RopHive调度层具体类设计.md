# RopHive 调度层具体类实现设计

本文档严格以 `docs/Chinese/platform/RopHive调度层设计.md` 为唯一设计输入，将其抽象落为可实现的“类 + 数据结构 + 线程安全策略”的工作文档；除多线程竞态与工程落地细节外，不引入额外调度策略（例如给任务增加老化/优先级之类的参数）。

---

## 0. 目标与非目标

### 0.1 目标

- 给出 **最小闭环**：`Hive` + `Worker` + `Task` + 全局池 + 本地双端队列 + 定时器 + 唤醒机制 + Work-Stealing。
- 让后续代码实现时能“照着写”：每个类的职责、成员、方法、线程安全边界、关键算法的伪代码与状态机。
- 解决理论设计中留白的点：多线程竞态、跨线程投递、timer_heap 选择策略的可实现性、wakeup 触发条件与粒度。

### 0.2 非目标

- 不讨论 UI 框架层、业务层任务模型的扩展（返回值、取消、优先级、老化、deadline 等）。
- 不引入除理论设计规定之外的新调度策略。
- 不把平台 API 细节展开到“逐平台实现代码”；仅给出接口与责任边界。

---

## 1. 命名与总体结构

仓库现状中 `RopHive` 已作为命名空间使用（例如 `RopHive::IEventLoopCore`），为了避免“命名空间名与类名同名”带来的可读性问题，本文采用如下命名约定：

- 模块命名空间：`namespace RopHive { ... }`（沿用现有）
- 调度器中枢类：`RopHive::Hive`
- 蜜蜂 Worker 类：`RopHive::HiveWorker`
- 计算 worker 建议在本文档中单独作为一类执行体描述（`ComputeWorker` 概念），因为它的 `run()` 与 IO-worker 的 1~7 节拍差异很大；工程实现上可以继续复用 `HiveWorker + ComputeBackend`，但文档结构上单列更清晰。

文档中“RopHive/蜂巢”指代该模块与系统，“Hive”指代具体类。

---

## 2. 核心概念与不变量

### 2.1 Task 分类

理论设计要求任务分为两类：

- `MicroTask`：IO 密集型 / 细粒度任务，主要由 IO-worker 执行，支持 `post` 与 `postDelayed`。
- `ComputeTask`：计算密集型任务，由 Compute-worker 以共享模式从 `GlobalComputeTaskPool` 获取；**不允许 delay 提交**（debug assert，release 忽略）。

### 2.2 队列分层（按执行绑定性）

每个 IO-worker（`HiveWorker`）持有：

- `private_queue`：强绑定当前 worker（不可窃取，优先级最高）。
- `local_dqueue`：可被其他 worker work-steal 的双端队列（Chase-Lev 风格，owner 从 Bottom LIFO，thief 从 Top FIFO）。
- `timer_heap`：该 worker 的延迟任务最小堆（仅 MicroTask）。
- `inbound_buffer`：跨线程投递到该 worker 的缓冲区（用于线程安全地把“目标 worker 的指令/任务”送达，让真正的写入发生在 owner 线程）。

Hive 持有：

- `GlobalMicroTaskPool`：全局 MicroTask 池，IO-worker “领用”。
- `GlobalComputeTaskPool`：全局 ComputeTask 池，Compute-worker “领用”。

### 2.3 Worker 运行节拍（强约束）

每个 IO-worker 的 `run()` 逻辑严格遵循理论设计的 1~7 步（系统消息优先 → inbound/private → timer → local → global → steal → wait），本文在具体实现章节给出可落地的伪代码与关键竞态处理。

---

## 3. 任务与指令的数据结构

### 3.1 `RopTask` / `MicroTask` / `ComputeTask`

为满足“分类 +（可选）归属信息 + 可执行体”的最小需要，推荐定义：

- `using TaskFn = std::function<void()>;`
- `struct MicroTask { TaskFn fn; };`
- `struct ComputeTask { TaskFn fn; };`

说明：
- 不在任务上增加优先级、老化、deadline 等额外字段。
- “投递到具体 worker”由 API 入参指定，而非塞进 Task 元数据（避免引入额外策略）。

### 3.2 `InboundCommand`：跨线程投递的统一载体

为避免跨线程直接修改 `private_queue/timer_heap/local_dqueue` 引入复杂锁，本设计把跨线程写入统一为“命令”进入 `inbound_buffer`，由 owner 线程在 Step-2 统一处理。

推荐：

```cpp
enum class InboundKind { PrivateTask, AddTimer, WakeupOnly, StopWorker };

struct InboundCommand {
  InboundKind kind;
  MicroTask task;                 // kind==PrivateTask
  /* Timer */ TimePoint deadline; // kind==AddTimer
  MicroTask timer_task;           // kind==AddTimer
};
```

约束：
- `InboundCommand` 只用于 IO-worker（Compute-worker 不需要 timer/private/local/steal 这套结构）。
- `postToWorker` 与 `postDelayed(选定worker)` 都转换为对该 worker 的 `inbound_buffer.enqueue(cmd)` + `wakeup()`。

---

## 4. 支撑组件（工具类）设计

本节描述实现 Hive/Worker 需要的关键工具类：线程池、双端无锁队列、全局池、MPSC inbound、timer_heap、睡眠集合、随机数。

### 4.1 `WorkerThreadPool`（Hive 内部线程池）

职责：在 `Hive::run()` 时为“除主线程 worker 外”的 worker 提供真实系统线程。

最小接口：

- `start(size_t n)`
- `submit(std::function<void()>)` 或为每个 worker 提供“专属线程启动”
- `stopJoin()`

实现要点：
- 线程创建/销毁由 `Hive::run()` 管控；支持 `Hive::attach()` 在运行中追加 worker 时扩容。
- 不需要复杂任务窃取/队列：worker 自己 run loop 就是线程主函数。

### 4.2 `ChaseLevDeque<MicroTask>`（local_dqueue）

用途：IO-worker 的 `local_dqueue`，提供 owner push/pop bottom + thief steal top 的 lock-free 双端队列模型。

关键点（落地所需的最小语义）：

- 单 owner 线程：`pushBottom`, `popBottom`
- 多 thief 线程：`stealTop`
- 容量上限 `capacity`（理论设计要求 DQueue 具有上限；满时 `postToLocal` 退化为 `hive.post` 全局投递）

推荐 API：

- `bool tryPushBottom(MicroTask t)`  // 满返回 false
- `std::optional<MicroTask> tryPopBottom()`
- `std::optional<MicroTask> tryStealTop()` // thief 随机受害者调用
- `size_t approxSize() const`        // 允许近似值（调试/统计用）
- `size_t remainingSpace() const`    // 队列剩余空间（上限队列专用）

实现建议：
- Chase-Lev 环形缓冲区 + `top/bottom` 原子索引 + CAS steal。
- 只要满足“owner 无锁、thief CAS 窃取、并发正确”，即可。

### 4.3 `MPSCInboundQueue<InboundCommand>`（inbound_buffer）

用途：多线程向某个 worker 投递“必须由该 worker 接收并落地”的命令。

必备语义：

- 多生产者（其他 worker、外部线程、Hive）并发 `enqueue(cmd)`
- 单消费者（owner worker）在 Step-2 `drainAll(vec)` 或 `popAll(list)`

推荐实现策略（简洁且高效）：

- 基于无锁链表的 MPSC：生产者用 `atomic<Node*> head` push；消费者用 `exchange(nullptr)` 一次性拿走整条链，再逆序/整理。
- 或者使用 `std::mutex` + `std::vector/deque`（实现简单但开销更大）；若用 mutex，仍建议 drain 时“swap 出来再处理”减少持锁时间。

### 4.4 `TimerHeap`（timer_heap）

用途：单 worker 的 MicroTask 延迟执行。

要求来自理论设计：
- 只在 worker 自己的循环里触发（Step-3）。
- Hive 的 `postDelayed` 选择策略需要读取 `timer_heap_.size()`。

落地策略：
- `timer_heap` 本体严格只在 owner 线程修改，避免复杂锁。
- 另维护一个 `std::atomic<uint32_t> timer_task_count_`：
  - 入堆时 `fetch_add(1)`
  - 出堆执行/丢弃时 `fetch_sub(1)`
  - Hive 读取 `timerTaskCount()` 作为 size 的近似替代（足够支撑“两随机比较”策略）。

推荐 API：

- `void addTimer(TimePoint deadline, MicroTask task)`（仅 owner 线程调用）
- `void runExpired(TimePoint now, size_t budget = unlimited)`
- `int computeNextTimeoutMs(TimePoint now)`（无任务返回 -1）
- `uint32_t approxSize() const`（返回 `timer_task_count_`）

跨线程加 timer：
- 统一走 `InboundCommand{AddTimer,...}`，由 Step-2 解析后调用 `TimerHeap::addTimer`。

### 4.5 `GlobalTaskPool`（全局池：micro/compute）

理论设计给出的必要特征：
- IO-worker 在 Step-5 **Batch Pop**（例如 16 个）以降低锁开销。
- worker 内部调用 `hive.post` 时建议先进入 worker 的本地缓冲，最后批量 flush（Step-6/7 间的“6.5”）。

落地建议（先保证正确性与接口，再优化实现）：

```cpp
class GlobalMicroTaskPool {
public:
  void push(MicroTask);
  void pushBatch(std::span<const MicroTask>);
  size_t tryPopBatch(std::span<MicroTask> out); // 返回实际拿到数量
};
```

命名对齐说明：
- 理论设计里提到的 `postBatch`，在这里对应“对外的 `Hive::postMicroBatch(...)` + 对内的 `GlobalMicroTaskPool::pushBatch(...)`”这一对接口。

实现可以是：
- `std::mutex + std::deque`：实现最简单；配合 batch pop/push，锁竞争可接受。
- 后续如需优化，可替换为无锁 MPMC 队列；对上层不破坏接口。

Compute 池同理，但 Compute-worker 侧通常只需要 `tryPop` 或 `tryPopBatch` + condvar 通知即可。

### 4.6 `SleepingWorkerSet`（只唤醒一个空闲 worker）

理论设计要求：
- Hive 维护 `sleeping_io_workers` 位图/列表；非空则任选其一唤醒；为空则不唤醒。
- 每个 worker 写自己的 sleep 标志，hive 读全部；标志用 `atomic<bool>` 保护。

落地方案（推荐）：

- Hive 持有 `std::vector<std::atomic<bool>> io_sleeping_` 与 `compute_sleeping_`（按 worker_id 索引）。
- worker 在进入 Step-7 阻塞前：`io_sleeping_[id].store(true, release)`
- worker 从阻塞返回后：`store(false, release)`
- Hive 在 `post` 时扫描（或用 lock-free 栈维护空闲列表）：
  - 扫描简单但 O(n)；n 通常较小可接受。
  - 若要求“唤醒路径无锁”（尤其是 compute），推荐用无锁 Treiber 栈/MPSC 队列维护空闲 worker id：
    - worker 入睡前先做 `sleeping.compare_exchange_strong(false, true, acq_rel)`，成功才把 `id` push 进无锁栈（避免重复入栈）。
    - Hive `wakeOneIdle*Worker()`：pop 一个 `id`，再二次校验 `sleeping[id].load(acquire)==true` 后调用 `wakeup()`；若校验失败则继续 pop 直到成功或空。
    - worker 被唤醒并从 wait 返回后 `sleeping.store(false, release)`。

无论采用哪种，必须满足“只唤醒一个空闲 worker”的语义。

### 4.7 `FastRng`（随机选择邻居/两随机策略）

用途：
- `postDelayed`：随机选两个 IO-worker。
- Work Stealing：随机挑选受害者。

要求：
- 每 worker 一个轻量 RNG（避免全局锁）。
- 允许伪随机即可（xorshift/pcg32 都可）。

---

## 5. `Hive`（调度器中枢）具体类设计

### 5.1 职责

- 管理 worker 生命周期：attach/detach/run（首个 worker 在主线程）。
- 持有并提供全局任务池（micro/compute）。
- 提供对外 API：`post/postDelayed/postToWorker`。
- 维护 worker 列表供 stealing 与“唤醒一个空闲 worker”使用。

### 5.2 成员（建议）

```cpp
class Hive {
  // --- lifecycle
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};

  // --- worker registry
  mutable std::mutex workers_mu_;
  std::vector<std::shared_ptr<HiveWorker>> io_workers_;       // 仅 IO-worker
  std::vector<std::shared_ptr<ComputeWorker>> compute_workers_;
  std::atomic<uint32_t> next_worker_id_{0};

  // --- global pools
  GlobalMicroTaskPool global_micro_;
  GlobalComputeTaskPool global_compute_;

  // --- sleep flags (indexed by worker_id)
  std::vector<std::atomic<bool>> io_sleeping_;
  std::vector<std::atomic<bool>> compute_sleeping_;

  // --- threads
  WorkerThreadPool pool_;
};
```

说明：
- `io_sleeping_`/`compute_sleeping_` 的容量与 worker_id 关联；动态扩容时要保证向量 reallocate 不造成悬挂引用（建议：只在 `workers_mu_` 内扩容，并且 worker 只通过 Hive 提供的访问器读写对应 `atomic<bool>` 指针/引用）。

### 5.3 对外方法（建议签名）

- `void attach(std::shared_ptr<HiveWorker> worker)`  
  - 仅做登记与分配 id，不启动线程。
  - 第一个 attach 的 IO-worker 将作为主线程 worker（运行在 `run()` 调用线程）。

- `void attach(std::shared_ptr<ComputeWorker> worker)`
  - 计算 worker 的 `run()` 与 IO-worker 完全不同，建议单独类型承载（见第 8 节）。

- `void detachIoWorker(uint32_t worker_id)`  
  - 允许在运行期 detach 非主线程 worker。
  - 不保证回收其队列中的 task（符合理论设计“hive 不负责其中 task 的回收”）。

- `void run()`  
  - 建立并启动线程：主线程执行第一个 IO-worker 的 `run()`；其余 worker 分配线程启动 `run()`。

- `void requestStop()`  
  - 触发 `stop_requested_=true`，并唤醒所有 worker 退出。

任务投递 API：

- `void postMicro(MicroTask task)`  
  - 外部线程：直接 push 到 `global_micro_`，并“只唤醒一个空闲 IO-worker”。
  - 若从某个 IO-worker 线程调用：推荐走 worker 的 outbound buffer（见 6.5）。

- `void postMicroBatch(std::span<const MicroTask> tasks)`  
  - 对应理论设计的 `postBatch`。
  - 外部线程：一次性 `global_micro_.pushBatch(tasks)`，并“只唤醒一个空闲 IO-worker”。
  - worker 线程：可直接 `global_micro_.pushBatch(tasks)`（本线程已在跑），或先进入 outbound buffer，最后在 6.5 flush。

- `void postCompute(ComputeTask task)`  
  - push 到 `global_compute_`，并唤醒一个空闲 compute-worker。

- `void postComputeBatch(std::span<const ComputeTask> tasks)`  
  - compute 全局池的 batch push；用于高频提交场景降低锁/唤醒开销。

- `void wakeOneIdleComputeWorker()`  
  - 语义同 `wakeOneIdleIoWorker()`，但建议实现为“无锁唤醒路径”（见 4.6 的无锁空闲栈/MPSC 方案），以保证 compute 提交高频时不被锁竞争放大。

- `void postDelayed(MicroTask task, Duration delay)`  
  - 选择目标 IO-worker（见 5.4），投递 `InboundCommand(AddTimer)` 到其 inbound，立即 wakeup 目标 worker。

- `void postToWorker(uint32_t worker_id, MicroTask task)`  
  - 投递 `InboundCommand(PrivateTask)` 到目标 worker inbound，立即 wakeup。

说明：
- 文档中把 `post` 拆为 `postMicro/postCompute` 是为了与理论设计的“双全局池”一致；若保留单一 `post(Task)`，则必须有一个明确的分类入口（例如 `post(TaskFn fn, TaskKind kind)`），否则无法满足“ComputeTask 投递到 GlobalComputeTaskPool”。

### 5.4 `postDelayed` 的 worker 选择（两随机择小）

严格按理论设计：

- IO-worker 数量为 1：直接选该 worker。
- IO-worker 数量 ≥ 2：
  1. 随机选出两个 IO-worker（A、B）
  2. 比较它们的 `timerTaskCount()`（近似 size）
  3. 选择更小者

并发注意点：
- worker 的 timer_heap 本体不跨线程读写；Hive 只读取 `atomic` 计数器，因此无锁且竞态可控。

### 5.5 “只唤醒一个空闲 worker”策略

对 `postMicro`（外部线程）：

1. `global_micro_.push(task)`
2. `wakeOneIdleIoWorker()`：
   - 扫描 `io_sleeping_`，找到任一 `true` 的 worker_id
   - 调用该 worker 的 `wakeup()`（线程安全）
   - 若无空闲 worker，则不唤醒（说明已有 worker 在跑）

注意：
- “只唤醒一个”是语义要求；不要 broadcast。
- `io_sleeping_` 可能出现短暂误差（worker 醒来刚清标志），最多导致一次多余 wakeup，可接受。

---

## 6. `HiveWorker`（IO-worker）具体类设计

### 6.1 职责

- 绑定一个系统线程（主线程或线程池线程）。
- 持有一个 `IEventLoopCore`：负责 `runOnce(timeout)` 与原始事件派发。
- 维护 private/local/timer/inbound 四层结构，按 1~7 节拍运行。
- 提供对外/对内 `postSelf/postToLocal/postSelfDelayed/postToHive` 等接口。
- 提供线程安全的 `wakeup()`。

### 6.2 成员（建议）

```cpp
class HiveWorker : public std::enable_shared_from_this<HiveWorker> {
  Hive* hive_ = nullptr;
  uint32_t worker_id_ = 0;

  // core / sources
  std::unique_ptr<IEventLoopCore> core_;
  std::unique_ptr<WakeupWorkerWatcher> wakeup_watcher_;

  // queues
  std::deque<MicroTask> private_queue_;          // 仅 owner 线程访问
  ChaseLevDeque<MicroTask> local_dqueue_;        // owner + thieves
  TimerHeap timers_;                              // 仅 owner 线程访问
  MPSCInboundQueue<InboundCommand> inbound_;     // MPSC -> owner drain

  // outbound buffer for hive.post called inside worker
  std::vector<MicroTask> outbound_global_buffer_; // 仅 owner 线程访问
  bool enable_outbound_batch_ = true;

  // state
  std::atomic<bool> stop_{false};
  std::atomic<bool> sleeping_{false};           // 写入 Hive 的 io_sleeping_[id] 也可复用
  FastRng rng_;
};
```

说明：
- `private_queue_` 仅在 owner 线程读写；跨线程投递进入 `inbound_`，由 Step-2 合并。
- `timers_` 仅 owner 修改；跨线程加 timer 也走 `inbound_`。
- `local_dqueue_` 必须支持 thief 窃取。

### 6.3 Watcher 体系：`WorkerWatcher` / `WakeupWorkerWatcher`

理论设计要求：
- worker 持有 wakeup 的 eventsource，注册方式类似 EventLoop 的 watcher；这里称 `WorkerWatcher`。
- 任何 EventSource 都平台相关；注册进 worker 的 core 需要通过 enum 判断 source 与平台是否相同，否则丢弃并 warning；debug 下可 assert。
- 唯一可以唤醒 worker 的方法为 `wakeup()`，且线程安全。

落地建议：

- `class WorkerWatcher`：持有 `HiveWorker&`，提供 `start()/stop()`，内部调用 `core_->addSource/removeSource`。
- `class WakeupWorkerWatcher : public WorkerWatcher`：
  - 内部封装平台 wakeup 机制（Linux: eventfd；Windows: PostThreadMessage；Mac: kqueue user event/pipe 等）
  - 对外提供 `void notify()`，可在任意线程调用
  - 同时提供一个 `IEventSource`，当被 core_ 派发时执行“读清 wakeup 事件 + no-op”（只用于打断阻塞）

### 6.4 对外/对内方法（建议）

生命周期：
- `void bind(Hive& hive, uint32_t worker_id)`：由 Hive 在 attach/run 过程中调用
- `void requestStop()`：设置 `stop_=true`，并 `wakeup()`
- `void run()`：执行 1~7 节拍循环，直到 `stop_` 或 `hive.stop_requested_`

投递：
- `void postSelf(MicroTask t)`：`private_queue_.push_back(t)`（仅 owner 线程调用；跨线程用 `Hive::postToWorker`）
- `void postToLocal(MicroTask t)`：
  - 若 `local_dqueue_.tryPushBottom(t)` 成功：结束
  - 否则（满）：退化为 `hive_->postMicro(std::move(t))`
- `void postSelfDelayed(MicroTask t, Duration d)`：
  - 仅 owner 线程：`timers_.addTimer(now+d, t)` 并更新计数
  - 若允许跨线程调用：必须转换为 `InboundCommand(AddTimer)` 投递 inbound（建议直接禁止跨线程调用该 API，统一用 Hive 的 `postDelayed`）

唤醒：
- `void wakeup()`：`wakeup_watcher_->notify()`（线程安全）

统计：
- `uint32_t timerTaskCount() const`：返回 `timers_.approxSize()`（原子计数）
- `uint32_t id() const`

### 6.5 “worker 内调用 hive.post 的缓冲区”落地

理论设计要求：
- 当 `hive.post` 在 worker 内调用时，先进入 worker 缓冲区。
- 在循环的最后（第 7 步前）通过 `Hive::postMicroBatch(...)`（对内落为 `GlobalMicroTaskPool::pushBatch(...)`）递交给全局池并清空。

落地方案：

- Hive 提供 `postMicro` 两条路径：
  1. 外部线程/非 worker 线程：直接 `global_micro_.push()` + `wakeOneIdleIoWorker()`
  2. worker 线程：调用 `HiveWorker::bufferGlobal(MicroTask)`（写入 `outbound_global_buffer_`，不立刻加锁全局池）

如何判断“当前线程是否 worker 线程”：
- 使用 `thread_local HiveWorker* tls_current_worker`：
  - 在 `HiveWorker::run()` 入口设置为 `this`，退出清空
  - `Hive::postMicro` 检测 `tls_current_worker != nullptr` 则走缓冲

Flush 时机（建议固定在 Step-6 与 Step-7 之间）：
- 若 `outbound_global_buffer_` 非空：
  - `hive_->postMicroBatch(outbound_global_buffer_)`（内部可直接落为 `global_micro_.pushBatch(...)`）
  - `outbound_global_buffer_.clear()`
  - 之后可选择 `wakeOneIdleIoWorker()`（但注意语义：这些任务本质来自 worker，本线程本来就在跑；一般只在确实需要并行扩散时才唤醒一个空闲 worker）

补充（关于“6.5 是否每轮都要跑”）：
- 从理论设计表述“每次循环都会经过第 7 步、在第 7 步前批上传”出发，建议把 6.5 作为**每轮循环的统一尾部动作**执行一次（内部仅在 buffer 非空时做实际工作）。
- 实现上建议用 scope-guard/defer，确保即使中途 `continue`，也会在本轮末尾执行一次 `flushOutboundGlobalBufferIfNeeded()`。

---

## 7. IO-worker `run()`：严格 1~7 节拍的可实现版本

### 7.1 关键辅助函数

- `bool hasImmediateWork()`：
  - `!private_queue_.empty()` 或 `local_dqueue_` 近似非空 或 `inbound_` 可能非空 或 `global_micro_` 可能非空（可不读）或 `timers_` 有到期
- `int computeTimeoutMs(now)`：
  - 若 `hasImmediateWork()`：返回 0（避免阻塞）
  - 否则返回 `timers_.computeNextTimeoutMs(now)`；无 timer 返回 -1

### 7.2 伪代码（按理论设计顺序）

```cpp
void HiveWorker::run() {
  tls_current_worker = this;
  core_->applyInitialChanges();

  uint32_t no_step5_counter = 0;

  while (!stop_ && !hive_->stop_requested_) {
    // Step-7 的 timeout 由本轮预计算，但实际 runOnce 在 Step-1
    auto now = Clock::now();
    int timeout = computeTimeoutMs(now);
    // ---- Step 1: 系统消息优先
    // 允许阻塞：timeout=-1 或 正数；如果本地有活则 timeout=0
    markSleeping(timeout != 0);          // 仅当会阻塞时才标记 sleeping
    core_->runOnce(timeout);
    markSleeping(false);
    // core_ 派发 raw events，回调中会调用 worker/hive 的 post 语义
    // ---- Step 2: 定向收割 (Inbound -> Private/Timer)
    InboundCommand cmd;
    while (inbound_.tryPop(cmd)) {
      switch (cmd.kind) {
        case InboundKind::PrivateTask: private_queue_.push_back(std::move(cmd.task)); break;
        case InboundKind::AddTimer: timers_.addTimer(cmd.deadline, std::move(cmd.timer_task)); break;
        case InboundKind::StopWorker: stop_ = true; break;
        default: break;
      }
    }
    // ---- Step 3: 定时器触发
    timers_.runExpired(Clock::now());
    // ---- Step 4: 本地消化 (Local DQueue)
    size_t local_budget = computeLocalBudget(); // N，超参数/分段函数
    MicroTask t;
    while (local_budget-- && local_dqueue_.tryPopBottom(t)) {
      t.fn();
    }
	    if (localHadWorkRecently) {
	      // 强制回到 Step-1（系统消息优先）
	      if (++no_step5_counter < global_probe_interval) {
	        // 6.5：每轮循环末尾都要有机会 flush
	        flushOutboundGlobalBufferIfNeeded();
	        continue;
	      }
	    }
    no_step5_counter = 0;
    // ---- Step 5: 全局支援 (Batch Pop -> local)
    std::array<MicroTask, kBatch> batch;
    size_t n = hive_->global_micro_.tryPopBatch(batch);
    if (n > 0) {
      for (i=0; i < n; i++) local_dqueue_.tryPushBottom(std::move(batch[i])); // 若失败则回投 global
      // 跳回 Step-1，必要时 timeout=0 让 wakeup 旁路
      flushOutboundGlobalBufferIfNeeded();
      continue;
    }
    // ---- Step 6: 窃取 (Work Stealing)
    tryStealOnceOrFew()

    // ---- Step 6.5: 批上传（worker 内 hive.post 缓冲）
    flushOutboundGlobalBufferIfNeeded();
    // ---- Step 7: 深度睡眠由下一轮 Step-1 的 runOnce(timeout) 承担
    // 这里不直接 wait，只是回到 while 顶部重新计算 timeout
  }

  tls_current_worker = nullptr;
}
```

说明：
- “Step-7 与 Step-1 是同一个事情”在实现上就是：在 while 顶部计算 timeout，然后 Step-1 调用 `runOnce(timeout)`。
- `markSleeping(true)` 只在确实可能进入内核阻塞时设置（timeout != 0），符合“sleeping_io_workers 代表空闲”的语义。
- `global_probe_interval = 47`（理论设计暂定），用于“即便 local 持续有活，也周期性尝试 Step-5 全局支援”。

### 7.3 Work Stealing 的最小实现策略

受害者选择：
- 从 Hive 的 `io_workers_` 随机挑选（避开自己）。
- 尝试若干次（例如 2~4 次），成功一次即返回。

窃取动作：
- 调用受害者 `local_dqueue_.tryStealTop(out)`（内部 CAS）。
- 成功后把偷来的任务放入自己的 `local_dqueue_`（或直接执行，但建议放入 local 保持节拍一致）。

---

## 8. `ComputeWorker`（计算 worker）具体类设计

理论设计里“计算类 worker”的关键约束是：
- 从 `GlobalComputeTaskPool` 共享取任务执行
- 通过多线程同步手段模拟 syscall 等待（例如条件变量），以便在“无活”时阻塞、在 `postCompute` 时被唤醒
- 不参与 MicroTask 的 work stealing；ComputeTask 不允许 delay

计算 worker 在语义上与 IO-worker 的 `run()` 完全不同：它不处理 IO 事件，不需要 1~7 节拍，也不参与 MicroTask 的 stealing/定时器/私有队列等结构。因此本文档将其单独列出为 `ComputeWorker`。

实现选项（不改变调度语义）：
- **选项 A（推荐）**：实现独立 `ComputeWorker` 类（最清晰，结构最小）。
- **选项 B（复用）**：仍使用 `HiveWorker`，但其 `IEventLoopCore`/backend 为 `ComputeBackend`（condvar wait），并在 `run()` 内根据 backend 类型走 compute loop。文档中以 `ComputeWorker` 的形态描述其成员/方法，便于实现与测试。

### 8.1 成员与方法（建议）

```cpp
class ComputeWorker {
public:
  using Task = ComputeTask;

  void bind(Hive& hive, uint32_t worker_id);
  void requestStop(); // 线程安全：设置 stop 并 wakeup
  void wakeup();      // 线程安全：唤醒 wait
  void run();         // 仅 compute loop

  uint32_t id() const;
  bool isSleeping() const;

private:
  Hive* hive_ = nullptr;
  uint32_t worker_id_ = 0;

  std::atomic<bool> stop_{false};
  std::atomic<bool> sleeping_{false}; // 同步到 Hive::compute_sleeping_[id]

  // 模拟 syscall 等待/唤醒（condvar）
  std::mutex mu_;
  std::condition_variable cv_;
};
```

说明：
- compute worker 不需要 `private_queue/local_dqueue/timer_heap/inbound_buffer`。
- compute worker 的等待是“无活才阻塞”；被唤醒后立即回到“取 compute 池任务”。

### 8.2 `run()` 伪代码（compute loop）

```cpp
void ComputeWorker::run() {
  while (!stop_ && !hive_->stop_requested_) {
    std::array<ComputeTask, kBatch> batch;
    size_t n = hive_->global_compute_.tryPopBatch(batch);
    if (n > 0) {
      sleeping_.store(false, std::memory_order_release);
      for (size_t i = 0; i < n; i++) batch[i].fn();
      continue;
    }

    // 无活：登记睡眠并阻塞等待
    markSleepingAndPublishToHive(); // 建议走无锁路径：sleeping CAS 去重 + 无锁空闲栈/队列
    std::unique_lock lk(mu_);
    cv_.wait(lk);
    sleeping_.store(false, std::memory_order_release);
  }
}
```

### 8.3 与 Hive 的交互（唤醒语义）

- `Hive::postCompute/postComputeBatch`：
  1. 入队 `global_compute_`
  2. `wakeOneIdleComputeWorker()`：唤醒一个 sleeping compute worker（尽量无锁，见 4.6）

约束：
- ComputeTask 不支持 delay（从 API 入口禁止）。
- Compute worker 不参与 MicroTask 的 work stealing。

---

## 9. 线程安全与竞态处理（必须落地的点）

### 9.1 总原则：owner-only + inbound 命令化

- `private_queue`、`timer_heap` 的真实容器只在 owner 线程读写。
- 跨线程需要投递到 worker 的任何动作，都转换为 `inbound_buffer.enqueue(cmd)`，由 Step-2 统一落地。
- 这样可以避免：
  - 跨线程对 `std::priority_queue` 加锁/竞态
  - private/local/timer 三处的多锁交叉死锁

### 9.2 sleeping 标志的正确使用

理论语义是“空闲 worker 才在列表中”。

落地规则：
- 只有当 worker 即将以 `timeout != 0` 进入可能阻塞的 `runOnce(timeout)` 时，才把 `sleeping=true`。
- `runOnce` 返回后立刻 `sleeping=false`。
- wakeup 可能发生在 `sleeping=false` 的窗口，最多造成一次多余 wakeup，不影响正确性。

### 9.3 唤醒触发点（按理论表格）

- `hive.postMicro`（外部线程）：push global 后，唤醒一个空闲 IO-worker（若有）。
- `hive.postDelayed`：投递 AddTimer 到目标 worker inbound，立即 `wakeup(target)`。
- `hive.postToWorker`：投递 PrivateTask 到目标 worker inbound，立即 `wakeup(target)`。
- `worker.postToLocal`：仅当 worker 处于 sleep 状态才需要唤醒（若从外部线程调用该方法，建议禁止；统一使用 hive.postToWorker/postMicro）。
- `worker.postSelf`：仅当 worker 处于 sleep 状态才需要唤醒（同上，跨线程场景走 inbound）。

### 9.4 detach 的竞态边界

理论设计要求 Hive 可 detach 非主线程 worker，且不负责 task 回收。

落地建议（保证不 UAF）：
- Hive 的 `workers_` 存 `shared_ptr`，detach 只是从 registry 移除，并对 worker `requestStop()`。
- worker 自己退出 run 后释放资源。
- detach 后该 worker 队列中的任务可能被丢弃（自然析构），这与“不负责回收”一致；调用者需保证任务本身的闭包析构安全。

---

## 10. 与 `IEventLoopCore/IEventSource` 的对接点

本文不参考 EventLoop 文档，只遵循理论设计中出现的抽象：

- worker 持有 `IEventLoopCore`，并在 Step-1 调 `runOnce(timeout)`
- 原始事件被派发后，`IEventSource::dispatch` 的回调里会调用 worker/hive 的 post 语义产生任务
- worker 通过 `WorkerWatcher` 体系管理 source 的注册/注销
- 注册时必须检查 source 与 backend/platform 是否匹配，不匹配则 warning（debug 可 assert）

因此，worker 提供最小的 source 管理接口即可：

- `void attachSource(IEventSource*)`（仅 watcher 调用）
- `void detachSource(IEventSource*)`

---

## 11. 参数（理论设计中出现的“超参数”）

以下参数在理论设计中被提到但未定值，建议集中为 Hive/HiveWorker 的配置项：

- `global_probe_interval`：默认 47
- `global_batch_pop`：默认 16
- `local_budget`（Step-4 一次最多执行 N 个 local task）：与 DQueue 容量相关的分段函数，需要实测
- `steal_attempts`：每轮 Step-6 尝试窃取的次数（例如 2~4）
- `enable_outbound_batch_`：是否启用 6.5 批上传优化开关

这些只是“吞吐/响应性”调参点，不引入新的调度语义。
关于这边的参数，建议写进一个头文件内，在后期写完完整系统后，进行单测调试，选取众多数据，就像 AI 调参一样测试何组数据结果最优


---

## 12. 最小使用方式（与理论伪代码对齐）

```cpp
RopHive::Hive hive;

auto ui_worker = std::make_shared<RopHive::HiveWorker>(/* Win32EventLoopCore or platform core */);
auto io_worker = std::make_shared<RopHive::HiveWorker>(/* IO core */);
auto compute_worker = std::make_shared<RopHive::HiveWorker>(/* ComputeBackend core */);

hive.attach(ui_worker);      // 第一个 attach：主线程 worker
hive.attach(io_worker);      // 线程池 worker
hive.attach(compute_worker); // 计算 worker

hive.postMicro({[] { /* ... */ }});
hive.postDelayed({[] { /* ... */ }}, 10ms);
hive.postCompute({[] { /* heavy compute */ }});

hive.run();
```

---

## 13. 实现检查清单（写代码前的自检点）

- [ ] `postDelayed` 只接受 MicroTask；ComputeTask delay 在 debug assert / release ignore
- [ ] `timer_heap` 不跨线程加锁写入：跨线程全部走 inbound AddTimer
- [ ] `postToWorker` 不直接写 `private_queue`：跨线程走 inbound PrivateTask
- [ ] IO-worker 的 run 循环严格 1~7；Step-7 通过 Step-1 的 `runOnce(timeout)` 体现
- [ ] `sleeping_io_workers` 语义是“空闲才为 true”，并且 Hive 的 post 只唤醒一个
- [ ] local_dqueue 有上限，满则退化为全局投递（旁路 stealing）

---

## 14. 单测与基准（面向长期演进）

本节目标是让 RopHive 在“可测”与“可比”两条线上都能长期维护：一方面能用单测快速覆盖竞态与语义，另一方面能用基准稳定地度量吞吐/延迟，并可与 Tokio 等成熟运行时做对标。

### 14.1 先把系统设计成可测：注入点与可控性

要写出稳定且可重复的单测，建议在实现阶段就预留以下“注入点”（不改变调度语义，只提高可测性）：

- **Clock 注入**：Worker 的 timer 逻辑不要直接绑死 `std::chrono::steady_clock::now()`，而是通过 `IClock::now()` 或函数对象注入。单测中用 `ManualClock` 精确推进时间，避免 `sleep_for` 带来的不稳定。
- **RNG 注入**：两随机择小、steal victim 选择用 `FastRng`，但允许注入固定 seed/脚本 RNG，使测试可复现（例如每次都选同一对 worker，或按序遍历受害者）。
- **Wakeup 注入**：`WakeupWorkerWatcher` 在单测里可替换为 `FakeWaker`（只记录 notify 次数/最后一次时间），避免依赖 eventfd/PostThreadMessage。
- **Backend/Core 注入**：IO-worker 的 `IEventLoopCore` 允许注入一个 `FakeEventLoopCore`：
  - `runOnce(timeout)` 不做系统阻塞，只记录 timeout，并按测试脚本派发“伪事件”回调；
  - 用于验证“Step-1 系统消息优先”和“timeout 计算”的语义。
- **队列实现可替换**：`ChaseLevDeque` 与 `MPSCInboundQueue` 可在单测中用“正确性优先”的带锁版本替换，以便将失败定位到调度逻辑而不是 lock-free 实现；等算法稳定后再对 lock-free 版本做单独的并发正确性测试。

### 14.2 单测分层：组件单测 → 调度语义单测 → 并发压力回归

建议把测试拆成三层，避免“一个测试里包含一切”导致不可定位：

#### A) 组件单测（无真实线程或少线程）

目标：验证数据结构与局部逻辑的确定性。

- `TimerHeap`
  - 给定 `ManualClock`，插入 N 个 timer，推进时间，验证到期任务执行顺序与数量。
  - 验证 `approxSize()` 计数增减正确，且 Hive 读取无需加锁。
- `MPSCInboundQueue`
  - 多线程 enqueue + 单线程 drain：验证不丢不重，允许顺序不保证（如你采用无锁链表方案）。
- `ChaseLevDeque`
  - owner push/pop 与 thief steal 的并发正确性：重点测“边界条件”（只剩 1 个元素时的 steal/pop 竞争）。
  - 如果你先用带锁实现跑通调度，再单独给 lock-free 版本做更强的压力测试（见 C）。

#### B) 调度语义单测（少线程 + FakeCore/ManualClock）

目标：验证理论设计中明确的语义约束，不追求极限性能。

覆盖点建议（按理论设计的关键语义）：

- **Step 顺序不可破坏**：构造场景让 Step-2 私有任务与 Step-3 timer 到期、Step-4 local 有活、Step-5 global 有活同时存在，验证每轮循环的处理优先级符合 1→2→3→4→5→6→7。
- **postDelayed 两随机择小**：固定 RNG，控制两个候选 worker 的 `timerTaskCount()`，验证选择更小者。
- **ComputeTask 禁止 delay**：debug 下 assert（可用 death test/断言捕获），release 下忽略（任务不入队）。
- **DQueue 满退化到 global**：把 `local_dqueue` 填满，再 `postToLocal`，验证进入 `global_micro_`（并且 stealing 不会“旁路”全局处理）。
- **只唤醒一个空闲 worker**：准备多个 sleeping worker，调用一次 `postMicroBatch`，验证只调用一次 `wakeup()`（其余不唤醒）。

#### C) 并发压力与竞态回归（真线程 + Sanitizer）

目标：长期防止回归（尤其是 inbound、steal、sleep/wakeup 竞态）。

- 运行策略：
  - 长时间随机测试（例如 10~60 秒），随机执行 `postMicro/postToWorker/postDelayed`，混合 worker attach/detach（如果实现支持）。
  - 每个任务只做极小工作（比如 `counter.fetch_add(1)`），以扩大调度与队列操作的覆盖密度。
- 工具建议：
  - ThreadSanitizer（TSAN）用于抓数据竞争；
  - AddressSanitizer/UBSan 用于抓 UAF/未定义行为；
  - 在 CI 或本地 nightly 跑一次“长压测”。

### 14.3 基准测试：你应该测什么（否则和 Tokio 对比没意义）

基准测试至少分两类指标：

- **吞吐（ops/s）**：单位时间能完成多少个 task。
- **延迟（p50/p95/p99）**：从 post 到被执行的时间分布；尤其关注 tail latency（UI 响应相关）。

建议固定一组基准场景（每个场景都能在 RopHive 与 Tokio 中实现同构工作负载）：

1. `postMicro` 吞吐：多线程生产者持续 post，任务体是 `atomic++`。
2. `postToWorker` 延迟：跨线程定向投递到某个 worker 的 private_queue，测 post→run 的延迟分布。
3. `postDelayed` 精度与抖动：固定延迟（例如 1ms/5ms/16ms），测触发偏差（实际触发时间 - 期望时间）。
4. steal 压力：制造一部分 worker 过载（大量 local），另一部分空闲，测整体吞吐与空闲 worker “偷到活”的比例。
5. 混合负载：MicroTask 与 ComputeTask 同时高压，观察 UI-worker（主线程）是否仍保持低延迟（这才是该调度层的核心价值）。

为保证结果可信，基准程序需要做这些“测量卫生”：

- 预热（warmup）一段时间再计时。
- 任务体尽量避免 IO/日志；必要时把结果写入内存，最后一次性输出。
- 固定线程数、固定亲和性（可选但强烈建议，至少在 Linux 对标时做）。
- 关闭/固定动态频率影响（至少记录 CPU governor 与 turbo 状态，否则对比无意义）。
- 统计多次 run 的方差，不只看一次结果。

### 14.4 与 Tokio 对标：怎么做到“尽量公平”

对标原则：对比“调度器开销”，而不是对比“任务做了多少业务逻辑”。

建议的 Tokio 对标方式（Linux 上最可比）：

- 使用 `tokio::runtime::Builder::new_multi_thread().worker_threads(N)` 固定 worker 数。
- MicroTask 对应：`tokio::spawn(async move { counter.fetch_add(1, Relaxed); })`（或用 `spawn_blocking` 对应 ComputeTask，但要注意它是独立 blocking 池，语义不同；若要严格对齐，ComputeTask 更接近“专用线程池”而不是 Tokio 的 work-stealing executor）。
- `postToWorker` 的“定向”语义 Tokio 原生不强：可以用“每个 worker 一个 mpsc channel + 固定线程消费”来模拟 private_queue（这更像你的 IO-worker private/inbound 结构）。
- `postDelayed` 对应：`tokio::time::sleep(delay).await` 后执行。

对标时建议输出同一套指标：

- ops/s
- p50/p95/p99 延迟（post→执行）
- wakeup 相关指标（可选）：RopHive 记录每个 worker 的 wakeup 次数；Tokio 可用 `perf`/trace 或仅对比宏观延迟

重要提示：
- Tokio 在任务模型（future/async）与内存分配策略上与 `std::function<void()>` 不同，对比时要尽量让“任务体相同、任务数量相同、线程数相同”，并在报告中明确差异来源。

### 14.5 推荐的目录/构建组织（不依赖第三方也能跑起来）

为了避免引入依赖阻碍落地，建议先做到“无第三方也能运行”：

- `tests/`：最小自研测试框架（断言 + test registry）或直接用 CTest 驱动多个小可执行文件。
- `bench/` 或 `example/bench_*`：基准可执行文件，支持参数 `--threads --duration --batch --mode`，输出 CSV/JSON。

等调度稳定后，再考虑引入成熟框架（例如 GoogleTest/GoogleBenchmark）以提升体验，但这不影响你现在把“可测性注入点”先设计好。
