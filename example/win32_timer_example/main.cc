#include <log.hpp>
#include <platform/schedule/hive.h>
#include  <platform/windows/schedule/watcher/win32_worker_timer.h>
#include <platform/schedule/io_worker.h>
#include <chrono>

#ifdef __linux__
#define DEFAULT_BACKEND BackendType::LINUX_EPOLL
#endif
#ifdef __APPLE__
#define DEFAULT_BACKEND BackendType::MACOS_KQUEUE
#endif
#ifdef _WIN32
#include <ws2tcpip.h>
#define DEFAULT_BACKEND BackendType::WINDOWS_IOCP
WSADATA wsaData;
#endif


int main(int argc, char* argv[]) {
    using namespace RopHive;

#if defined(_WIN32)
    int ret = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (ret != 0) {
        LOG(ERROR)("WSAStartup failed: %d\n", ret);
        return 1;
    }
#endif
    logger::setMinLevel(LogLevel::DEBUG);
    Hive::Options opt;
    opt.io_backend = DEFAULT_BACKEND;

    Hive hive(opt);
    auto worker = std::make_shared<IOWorker>(opt);
    hive.attachIOWorker(worker);

    hive.postToWorker(0, [worker] {
        auto* self = IOWorker::currentWorker();
        if (!self) return;
        using namespace std::chrono_literals;

        auto watcher = std::make_shared<Windows::Win32WorkerTimerWatcher>(*self, [worker] {
            LOG(INFO)("Timer fired after 1 second");

        });
        self->adoptWatcher(watcher);
        watcher->setSpec(1ns, 100ns);
        watcher->start();
        LOG(INFO)("Timer started with 1 second initial delay and 1 second interval");

    });

    hive.run();
#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
}