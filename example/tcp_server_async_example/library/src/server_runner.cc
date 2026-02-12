#include "server_runner.h"

#include "app.h"

#include <csignal>
#include <execinfo.h>
#include <exception>
#include <memory>
#include <string>

#include <log.hpp>
#include <platform/schedule/hive.h>
#include <platform/schedule/io_worker.h>

#ifdef __linux__
#define DEFAULT_BACKEND BackendType::LINUX_EPOLL
#endif
#ifdef __APPLE__ 
#define DEFAULT_BACKEND BackendType::MACOS_KQUEUE
#endif
#ifdef _WIN32
#define DEFAULT_BACKEND BackendType::WINDOWS_IOCP
#endif

namespace tcp_server_async_example {

static int parseInt(const char* s, int fallback) {
    if (!s) return fallback;
    try {
        return std::stoi(std::string(s));
    } catch (...) {
        return fallback;
    }
}

static void installCrashHandler() {
    std::signal(SIGSEGV, +[](int) {
        const char msg[] = "SIGSEGV\n";
        ::write(2, msg, sizeof(msg) - 1);
        void* frames[64];
        const int n = ::backtrace(frames, 64);
        ::backtrace_symbols_fd(frames, n, 2);
        std::_Exit(139);
    });
}

static void installTerminateHandler() {
    std::set_terminate([]() {
        const char prefix[] = "std::terminate called\n";
        ::write(2, prefix, sizeof(prefix) - 1);
        if (auto eptr = std::current_exception()) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::system_error& e) {
                std::string msg = "uncaught std::system_error: code=" +
                                  std::to_string(e.code().value()) +
                                  " message=" + e.code().message() +
                                  " what=" + std::string(e.what()) + "\n";
                ::write(2, msg.data(), msg.size());
            } catch (const std::exception& e) {
                std::string msg = std::string("uncaught std::exception: ") + e.what() + "\n";
                ::write(2, msg.data(), msg.size());
            } catch (...) {
                const char msg[] = "uncaught non-std exception\n";
                ::write(2, msg, sizeof(msg) - 1);
            }
        }
        std::abort();
    });
}

int run(int argc, char** argv) {
    logger::setMinLevel(LogLevel::INFO);
    installCrashHandler();
    installTerminateHandler();

    ::RopHive::Hive::Options opt;
    opt.io_backend = ::RopHive::DEFAULT_BACKEND;

    const int worker_n = std::max(1, parseInt(argc > 1 ? argv[1] : nullptr, 4));

    ::RopHive::Hive hive(opt);
    for (int i = 0; i < worker_n; ++i) {
        hive.attachIOWorker(std::make_shared<::RopHive::IOWorker>(opt));
    }

    auto execs = std::make_shared<std::vector<std::shared_ptr<asyncnet::Executor>>>();
    execs->resize(static_cast<size_t>(worker_n));

    for (int i = 0; i < worker_n; ++i) {
        hive.postToWorker(static_cast<size_t>(i), [execs, i]() {
            auto* self = ::RopHive::IOWorker::currentWorker();
            if (!self) return;
            (*execs)[static_cast<size_t>(i)] = std::make_shared<asyncnet::Executor>(*self);
        });
    }

    hive.postToWorker(0, [&hive, execs, worker_n]() {
        auto* self = ::RopHive::IOWorker::currentWorker();
        if (!self) return;
        auto exec = (*execs)[0];
        if (!exec) {
            exec = std::make_shared<asyncnet::Executor>(*self);
            (*execs)[0] = exec;
        }
        asyncnet::spawn(*exec, async_main(*exec, hive, worker_n, execs));
    });

    hive.run();
    return 0;
}

} // namespace tcp_server_async_example

