#include "asyncnet.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include <log.hpp>
#include <platform/linux/schedule/epoll_backend.h>
#include <platform/schedule/io_worker.h>
#include <platform/schedule/worker_watcher.h>

namespace asyncnet {

using ::RopHive::IOWorker;
using ::RopHive::IEventSource;
using ::RopHive::IWorkerWatcher;
using ::RopHive::Linux::EpollReadinessEventSource;

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

class WorkerSourceGuard : public IWorkerWatcher {
public:
    explicit WorkerSourceGuard(IOWorker& worker) : IWorkerWatcher(worker) {}
    void start() override {}
    void stop() override {}

    void attach(const std::shared_ptr<IEventSource>& src) { attachSource(src); }
    void detach(const std::shared_ptr<IEventSource>& src) { detachSource(src); }
};

void spawn(Executor& exec, Task<void> task) {
    task.detach(exec);
}

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

class AsyncFd : public std::enable_shared_from_this<AsyncFd> {
public:
    static std::shared_ptr<AsyncFd> create(Executor& exec, int fd, uint32_t events) {
        auto self = std::shared_ptr<AsyncFd>(new AsyncFd(exec, fd));
        self->init(events);
        return self;
    }

    ~AsyncFd() {
        guard_.detach(source_);
    }

    int fd() const noexcept { return fd_; }
    void setEvents(uint32_t events) { source_->setEvents(events); }

    struct Readable {
        std::shared_ptr<AsyncFd> self;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) { self->read_waiters_.push_back(h); }
        uint32_t await_resume() const noexcept { return self->last_events_; }
    };

    struct Writable {
        std::shared_ptr<AsyncFd> self;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) { self->write_waiters_.push_back(h); }
        uint32_t await_resume() const noexcept { return self->last_events_; }
    };

    Readable readable() { return Readable{shared_from_this()}; }
    Writable writable() { return Writable{shared_from_this()}; }

private:
    AsyncFd(Executor& exec, int fd)
        : exec_(exec), guard_(exec.worker), fd_(fd) {}

    void init(uint32_t events) {
        // The epoll callback can outlive this object if there are already pending events.
        // Guard against use-after-free by capturing a weak_ptr instead of `this`.
        auto weak = weak_from_this();
        source_ = std::make_shared<EpollReadinessEventSource>(
            fd_,
            events,
            [weak](uint32_t ev) {
                if (auto self = weak.lock()) {
                    self->onReady(ev);
                }
            });
        guard_.attach(source_);
    }

    void onReady(uint32_t events) {
        last_events_ = events;
        while (!read_waiters_.empty()) {
            auto h = read_waiters_.front();
            read_waiters_.pop_front();
            exec_.schedule(h);
        }
        while (!write_waiters_.empty()) {
            auto h = write_waiters_.front();
            write_waiters_.pop_front();
            exec_.schedule(h);
        }
    }

private:
    Executor& exec_;
    WorkerSourceGuard guard_;
    int fd_{-1};
    std::shared_ptr<EpollReadinessEventSource> source_;
    uint32_t last_events_{0};
    std::deque<std::coroutine_handle<>> read_waiters_;
    std::deque<std::coroutine_handle<>> write_waiters_;
};

class TcpStream : public std::enable_shared_from_this<TcpStream> {
public:
    TcpStream(Executor& exec, int fd)
        : exec_(exec), fd_(fd) {
        afd_ = AsyncFd::create(exec_, fd_, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
    }

    ~TcpStream() {
        // Detach from epoll before closing the fd to avoid events firing on a closed/reused fd.
        afd_.reset();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    Task<std::optional<std::string>> recvSome() {
        for (;;) {
            char buf[4096];
            const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n > 0) {
                co_return std::string(buf, buf + n);
            }
            if (n == 0) {
                co_return std::nullopt;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                co_await afd_->readable();
                continue;
            }
            co_return std::nullopt;
        }
    }

    Task<void> sendAll(std::string data) {
        out_.append(data);
        afd_->setEvents(EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLOUT);
        while (!out_.empty()) {
            const ssize_t n = ::send(fd_, out_.data(), out_.size(), MSG_NOSIGNAL);
            if (n > 0) {
                out_.erase(0, static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                co_await afd_->writable();
                continue;
            }
            co_return;
        }
        afd_->setEvents(EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
        co_return;
    }

private:
    Executor& exec_;
    int fd_{-1};
    std::shared_ptr<AsyncFd> afd_;
    std::string out_;
};

std::shared_ptr<TcpStream> wrapStream(Executor& exec, int fd) {
    return std::make_shared<TcpStream>(exec, fd);
}

static int setNonBlockingFd(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int makeListenSocketInternal(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG(FATAL)("socket failed: %s", std::strerror(errno));
        std::abort();
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (setNonBlockingFd(fd) != 0) {
        LOG(FATAL)("fcntl O_NONBLOCK failed: %s", std::strerror(errno));
        std::abort();
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG(FATAL)("bind failed: %s", std::strerror(errno));
        ::close(fd);
        std::abort();
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        LOG(FATAL)("listen failed: %s", std::strerror(errno));
        ::close(fd);
        std::abort();
    }
    return fd;
}

class TcpListener : public std::enable_shared_from_this<TcpListener> {
public:
    TcpListener(Executor& exec, int port)
        : exec_(exec), listen_fd_(makeListenSocketInternal(port)) {
        afd_ = AsyncFd::create(exec_, listen_fd_, EPOLLIN | EPOLLERR | EPOLLHUP);
    }

    ~TcpListener() {
        // Detach from epoll before closing the fd to avoid events firing on a closed/reused fd.
        afd_.reset();
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    Task<std::shared_ptr<TcpStream>> accept() {
        for (;;) {
            int client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_fd >= 0) {
                co_return std::make_shared<TcpStream>(exec_, client_fd);
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                co_await afd_->readable();
                continue;
            }
            co_return std::shared_ptr<TcpStream>{};
        }
    }

    Task<int> acceptFd() {
        for (;;) {
            int client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_fd >= 0) {
                co_return client_fd;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                co_await afd_->readable();
                continue;
            }
            co_return -1;
        }
    }

private:
    Executor& exec_;
    int listen_fd_{-1};
    std::shared_ptr<AsyncFd> afd_;
};

std::shared_ptr<TcpListener> listen(Executor& exec, int port) {
    return std::make_shared<TcpListener>(exec, port);
}

Task<std::shared_ptr<TcpStream>> accept(std::shared_ptr<TcpListener> listener) {
    co_return co_await listener->accept();
}

Task<int> acceptFd(std::shared_ptr<TcpListener> listener) {
    co_return co_await listener->acceptFd();
}

Task<std::optional<std::string>> recvSome(std::shared_ptr<TcpStream> stream) {
    co_return co_await stream->recvSome();
}

Task<void> sendAll(std::shared_ptr<TcpStream> stream, std::string data) {
    co_await stream->sendAll(std::move(data));
}

Task<std::shared_ptr<TcpStream>> connect(Executor& exec, std::string host, int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        co_return std::shared_ptr<TcpStream>{};
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        co_return std::shared_ptr<TcpStream>{};
    }

    int rc;
    do {
        rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    } while (rc != 0 && errno == EINTR);

    if (rc == 0) {
        co_return std::make_shared<TcpStream>(exec, fd);
    }
    if (errno != EINPROGRESS) {
        ::close(fd);
        co_return std::shared_ptr<TcpStream>{};
    }

    auto afd = AsyncFd::create(exec, fd, EPOLLOUT | EPOLLERR | EPOLLHUP);
    co_await afd->writable();

    int so_err = 0;
    socklen_t len = sizeof(so_err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0) {
        so_err = errno;
    }
    if (so_err != 0) {
        ::close(fd);
        co_return std::shared_ptr<TcpStream>{};
    }
    co_return std::make_shared<TcpStream>(exec, fd);
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
                           int port,
                           ConnectionHandler handler) {
    auto listener = asyncnet::listen(accept_exec, port);

    size_t rr = 0;
    for (;;) {
        const int client_fd = co_await asyncnet::acceptFd(listener);
        if (client_fd < 0) co_return;

        const size_t target = (rr++) % static_cast<size_t>(std::max(1, worker_n));
        hive.postToWorker(target, [execs, target, client_fd, handler]() mutable {
            auto* self = ::RopHive::IOWorker::currentWorker();
            if (!self) return;
            auto exec = (*execs)[target];
            if (!exec) return;

            auto stream = asyncnet::wrapStream(*exec, client_fd);
            asyncnet::spawn(*exec, handler(*exec, std::move(stream)));
        });
    }
}

} // namespace asyncnet
