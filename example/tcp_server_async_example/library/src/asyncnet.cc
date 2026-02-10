#include "asyncnet.h"

#include <deque>
#include <optional>
#include <string>
#include <utility>

#include <log.hpp>
#include <platform/network/ip_endpoint.h>
#include <platform/network/watcher/tcp_watchers.h>
#include <platform/schedule/hive.h>
#include <platform/schedule/io_worker.h>

namespace asyncnet {

using ::RopHive::IOWorker;
using ::RopHive::Network::IpEndpoint;
using ::RopHive::Network::IpEndpointV4;
using ::RopHive::Network::ITcpAcceptWatcher;
using ::RopHive::Network::ITcpConnectWatcher;
using ::RopHive::Network::ITcpConnectionWatcher;
using ::RopHive::Network::ITcpStream;
using ::RopHive::Network::TcpAcceptOption;
using ::RopHive::Network::TcpConnectOption;
using ::RopHive::Network::TcpConnectionOption;
using ::RopHive::Network::createTcpAcceptWatcher;
using ::RopHive::Network::createTcpConnectWatcher;
using ::RopHive::Network::createTcpConnectionWatcher;
using ::RopHive::Network::parseIpEndpoint;

void Executor::schedule(std::coroutine_handle<> h) {
    worker.postPrivate([this, h]() mutable {
        Executor* prev = tls_current_executor;
        tls_current_executor = this;
        try {
            h.resume();
        } catch (const std::system_error& e) {
            LOG(ERROR)("uncaught std::system_error during coroutine resume: code=%d (%s) what=%s",
                       e.code().value(),
                       e.code().message().c_str(),
                       e.what());
            std::terminate();
        } catch (const std::exception& e) {
            LOG(ERROR)("uncaught std::exception during coroutine resume: %s", e.what());
            std::terminate();
        } catch (...) {
            LOG(ERROR)("uncaught non-std exception during coroutine resume");
            std::terminate();
        }
        tls_current_executor = prev;
    });
    worker.wakeup();
}

void spawn(Executor& exec, Task<void> task) {
    task.detach(exec);
}

// ---- TaskGroup ----

struct TaskGroup::State {
    explicit State(Executor& executor) : exec(&executor) {}

    Executor* exec{nullptr};
    size_t active{0};
    std::optional<std::coroutine_handle<>> join_waiter;
};

Task<void> TaskGroup::childWrapper(std::shared_ptr<State> st, Task<void> task) {
    co_await task;
    st->active -= 1;
    if (st->active == 0 && st->join_waiter) {
        auto h = *st->join_waiter;
        st->join_waiter.reset();
        st->exec->schedule(h);
    }
    co_return;
}

TaskGroup::TaskGroup(Executor& exec)
    : state_(std::make_shared<State>(exec)) {}

TaskGroup taskGroup(Executor& exec) {
    return TaskGroup(exec);
}

void TaskGroup::spawn(Task<void> task) {
    if (!state_ || !state_->exec) return;
    state_->active += 1;
    auto st = state_;
    asyncnet::spawn(*state_->exec, TaskGroup::childWrapper(st, std::move(task)));
}

Task<void> TaskGroup::join() {
    if (!state_ || !state_->exec) co_return;
    if (state_->active == 0) co_return;

    struct Awaiter {
        std::shared_ptr<State> st;
        bool await_ready() const noexcept { return st->active == 0; }
        void await_suspend(std::coroutine_handle<> h) { st->join_waiter = h; }
        void await_resume() const noexcept {}
    };

    co_await Awaiter{state_};
    co_return;
}

// ---- TCP adapters built on new unified tcp watchers ----

class AsyncTcpStream : public std::enable_shared_from_this<AsyncTcpStream> {
public:
    static std::shared_ptr<AsyncTcpStream> create(Executor& exec, std::unique_ptr<ITcpStream> connected) {
        auto self = std::shared_ptr<AsyncTcpStream>(new AsyncTcpStream(exec));
        self->init(std::move(connected));
        return self;
    }

    ~AsyncTcpStream() {
        auto watcher = watcher_;
        if (!watcher) return;

        auto& worker = exec_.worker;
        auto cleanup = [watcher, &worker]() mutable {
            watcher->close();
            watcher->stop();
            worker.releaseWatcher(watcher.get());
        };

        if (IOWorker::currentWorker() == &worker) {
            cleanup();
        } else {
            worker.postPrivate(std::move(cleanup));
            worker.wakeup();
        }
    }

    Task<std::optional<std::string>> recvSome() {
        auto chunk = co_await recv_q_.pop();
        if (!chunk.has_value()) co_return std::nullopt;
        co_return std::move(*chunk);
    }

    Task<void> sendAll(std::string data) {
        struct LockAwaiter {
            AsyncTcpStream& self;
            bool await_ready() const noexcept { return !self.send_locked_; }
            void await_suspend(std::coroutine_handle<> h) { self.send_lock_waiters_.push_back(h); }
            void await_resume() noexcept { self.send_locked_ = true; }
        };

        struct Unlocker {
            AsyncTcpStream* self{nullptr};
            ~Unlocker() {
                if (self) self->unlockSend_();
            }
        };

        co_await LockAwaiter{*this};
        Unlocker unlock{this};

        out_.append(std::move(data));
        if (!watcher_) co_return;

        while (!out_.empty()) {
            auto res = watcher_->trySend(out_);
            if (res.err != 0) {
                recv_q_.close();
                send_ready_q_.close();
                watcher_->close();
                co_return;
            }

            if (res.n > 0) {
                out_.erase(0, res.n);
                continue;
            }

            if (res.would_block) {
                auto ready = co_await send_ready_q_.pop();
                if (!ready.has_value()) co_return;
                continue;
            }

            co_return;
        }
        co_return;
    }

private:
    explicit AsyncTcpStream(Executor& exec)
        : exec_(exec) {
        recv_q_.bind(exec_);
        send_ready_q_.bind(exec_);
    }

    void init(std::unique_ptr<ITcpStream> connected) {
        auto weak = weak_from_this();
        auto watcher_wp_box = std::make_shared<std::weak_ptr<ITcpConnectionWatcher>>();

        TcpConnectionOption opt;
        watcher_ = createTcpConnectionWatcher(
            exec_.worker,
            opt,
            std::move(connected),
            [weak](std::string_view chunk) {
                if (auto self = weak.lock()) {
                    self->recv_q_.push(std::string(chunk));
                }
            },
            [weak, watcher_wp_box]() {
                if (auto self = weak.lock()) {
                    self->recv_q_.close();
                    self->send_ready_q_.close();
                }
                if (auto* w = IOWorker::currentWorker()) {
                    if (auto watcher = watcher_wp_box->lock()) {
                        watcher->stop();
                        w->releaseWatcher(watcher.get());
                    }
                }
            },
            [weak, watcher_wp_box](int err) {
                LOG(WARN)("connection error=%d", err);
                if (auto self = weak.lock()) {
                    self->recv_q_.close();
                    self->send_ready_q_.close();
                }
                if (auto* w = IOWorker::currentWorker()) {
                    if (auto watcher = watcher_wp_box->lock()) {
                        watcher->stop();
                        w->releaseWatcher(watcher.get());
                    }
                }
            },
            [weak]() {
                if (auto self = weak.lock()) {
                    self->send_ready_q_.push(1);
                }
            });

        *watcher_wp_box = watcher_;
        exec_.worker.adoptWatcher(watcher_);
        watcher_->start();
    }

    void unlockSend_() {
        if (!send_lock_waiters_.empty()) {
            auto h = send_lock_waiters_.front();
            send_lock_waiters_.pop_front();
            exec_.schedule(h);
            return;
        }
        send_locked_ = false;
    }

private:
    Executor& exec_;
    std::shared_ptr<ITcpConnectionWatcher> watcher_;
    AsyncQueue<std::string> recv_q_;
    AsyncQueue<int> send_ready_q_;

    std::string out_;

    bool send_locked_{false};
    std::deque<std::coroutine_handle<>> send_lock_waiters_;
};

class AsyncTcpListener : public std::enable_shared_from_this<AsyncTcpListener> {
public:
    static std::shared_ptr<AsyncTcpListener> create(Executor& exec, IpEndpoint local_bind) {
        auto self = std::shared_ptr<AsyncTcpListener>(new AsyncTcpListener(exec, std::move(local_bind)));
        self->init();
        return self;
    }

    ~AsyncTcpListener() {
        auto watcher = watcher_;
        if (!watcher) return;

        auto& worker = exec_.worker;
        auto cleanup = [watcher, &worker]() mutable {
            watcher->stop();
            worker.releaseWatcher(watcher.get());
        };

        if (IOWorker::currentWorker() == &worker) {
            cleanup();
        } else {
            worker.postPrivate(std::move(cleanup));
            worker.wakeup();
        }
    }

    Task<std::shared_ptr<AsyncTcpStream>> accept() {
        auto connected = co_await acceptStream_();
        if (!connected) co_return std::shared_ptr<AsyncTcpStream>{};
        co_return AsyncTcpStream::create(exec_, std::move(connected));
    }

    Task<std::unique_ptr<ITcpStream>> acceptStream_() {
        auto v = co_await accepted_.pop();
        if (!v.has_value()) co_return std::unique_ptr<ITcpStream>{};
        co_return std::move(*v);
    }

private:
    AsyncTcpListener(Executor& exec, IpEndpoint local_bind)
        : exec_(exec), local_bind_(std::move(local_bind)) {
        accepted_.bind(exec_);
    }

    void init() {
        TcpAcceptOption opt;
        opt.local = local_bind_;
        opt.fill_endpoints = true;
        opt.reuse_addr = true;
        opt.set_close_on_exec = true;
        opt.tcp_no_delay = true;

        auto weak = weak_from_this();
        auto watcher_wp_box = std::make_shared<std::weak_ptr<ITcpAcceptWatcher>>();

        watcher_ = createTcpAcceptWatcher(
            exec_.worker,
            opt,
            [weak](std::unique_ptr<ITcpStream> stream) {
                if (auto self = weak.lock()) {
                    self->accepted_.push(std::move(stream));
                }
            },
            [weak, watcher_wp_box](int err) {
                LOG(WARN)("accept watcher error=%d", err);
                if (auto self = weak.lock()) {
                    self->accepted_.close();
                }
                if (auto* w = IOWorker::currentWorker()) {
                    if (auto watcher = watcher_wp_box->lock()) {
                        watcher->stop();
                        w->releaseWatcher(watcher.get());
                    }
                }
            });

        *watcher_wp_box = watcher_;
        exec_.worker.adoptWatcher(watcher_);
        watcher_->start();
    }

private:
    Executor& exec_;
    IpEndpoint local_bind_;
    AsyncQueue<std::unique_ptr<ITcpStream>> accepted_;
    std::shared_ptr<ITcpAcceptWatcher> watcher_;
};

std::shared_ptr<AsyncTcpListener> listen(Executor& exec, IpEndpoint local_bind) {
    return AsyncTcpListener::create(exec, std::move(local_bind));
}

std::shared_ptr<AsyncTcpListener> listen(Executor& exec, int port) {
    IpEndpointV4 bind_v4;
    bind_v4.ip = {0, 0, 0, 0};
    bind_v4.port = static_cast<uint16_t>(port);
    return AsyncTcpListener::create(exec, bind_v4);
}

Task<std::shared_ptr<AsyncTcpStream>> accept(std::shared_ptr<AsyncTcpListener> listener) {
    if (!listener) co_return std::shared_ptr<AsyncTcpStream>{};
    co_return co_await listener->accept();
}

Task<std::optional<std::string>> recvSome(std::shared_ptr<AsyncTcpStream> stream) {
    if (!stream) co_return std::nullopt;
    co_return co_await stream->recvSome();
}

Task<void> sendAll(std::shared_ptr<AsyncTcpStream> stream, std::string data) {
    if (!stream) co_return;
    co_await stream->sendAll(std::move(data));
}

Task<std::shared_ptr<AsyncTcpStream>> connect(Executor& exec, std::string host, int port) {
    const std::string addr = host + ":" + std::to_string(port);
    auto remote_opt = parseIpEndpoint(addr);
    if (!remote_opt.has_value()) co_return std::shared_ptr<AsyncTcpStream>{};

    AsyncQueue<std::unique_ptr<ITcpStream>> connected;
    connected.bind(exec);

    auto watcher_wp_box = std::make_shared<std::weak_ptr<ITcpConnectWatcher>>();
    TcpConnectOption opt;
    opt.fill_endpoints = true;
    opt.set_close_on_exec = true;
    opt.tcp_no_delay = true;

    auto watcher = createTcpConnectWatcher(
        exec.worker,
        *remote_opt,
        opt,
        [&connected](std::unique_ptr<ITcpStream> stream) {
            connected.push(std::move(stream));
            connected.close();
        },
        [&connected, watcher_wp_box](int err) {
            LOG(WARN)("connect watcher error=%d", err);
            connected.close();
            if (auto* w = IOWorker::currentWorker()) {
                if (auto cw = watcher_wp_box->lock()) {
                    cw->stop();
                    w->releaseWatcher(cw.get());
                }
            }
        });

    *watcher_wp_box = watcher;
    exec.worker.adoptWatcher(watcher);
    watcher->start();

    auto v = co_await connected.pop();
    if (!v.has_value() || !*v) co_return std::shared_ptr<AsyncTcpStream>{};
    co_return AsyncTcpStream::create(exec, std::move(*v));
}

Task<void> sleepFor(Executor& exec, std::chrono::milliseconds delay) {
    struct Awaiter {
        Executor& exec;
        std::chrono::milliseconds delay;
        bool await_ready() const noexcept { return delay.count() <= 0; }
        void await_suspend(std::coroutine_handle<> h) {
            Executor* exec_ptr = &exec;
            exec.worker.addTimer(::RopHive::Hive::Clock::now() + delay, [exec_ptr, h]() mutable {
                exec_ptr->schedule(h);
            });
            exec.worker.wakeup();
        }
        void await_resume() const noexcept {}
    };

    co_await Awaiter{exec, delay};
    co_return;
}

Task<void> serveRoundRobin(Executor& accept_exec,
                           ::RopHive::Hive& hive,
                           int worker_n,
                           std::shared_ptr<std::vector<std::shared_ptr<Executor>>> execs,
                           IpEndpoint local_bind,
                           ConnectionHandler handler) {
    auto listener = asyncnet::listen(accept_exec, std::move(local_bind));

    size_t rr = 0;
    for (;;) {
        auto connected = co_await listener->acceptStream_();
        if (!connected) co_return;

        const size_t target = (rr++) % static_cast<size_t>(std::max(1, worker_n));
        auto connected_box =
            std::make_shared<std::unique_ptr<ITcpStream>>(std::move(connected));

        hive.postToWorker(target, [execs, target, connected_box, handler]() mutable {
            auto* self = ::RopHive::IOWorker::currentWorker();
            if (!self) return;
            auto exec = (*execs)[target];
            if (!exec) return;

            auto connected_local = std::move(*connected_box);
            if (!connected_local) return;
            auto stream = AsyncTcpStream::create(*exec, std::move(connected_local));
            asyncnet::spawn(*exec, handler(*exec, std::move(stream)));
        });
    }
}

} // namespace asyncnet
