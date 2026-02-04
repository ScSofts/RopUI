#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <log.hpp>
#include <platform/linux/network/watcher/tcp_accept_watcher.h>
#include <platform/linux/network/watcher/tcp_connection_watcher.h>
#include <platform/schedule/hive.h>
#include <platform/schedule/io_worker.h>

using namespace RopHive;
using namespace RopHive::Linux;

static int makeListenSocket(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG(FATAL)("socket failed: %s", std::strerror(errno));
        std::abort();
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

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

static std::string peerToString(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return "unknown";
    }
    char ip[INET_ADDRSTRLEN]{};
    if (!::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip))) {
        std::snprintf(ip, sizeof(ip), "invalid");
    }
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

class EchoSession : public std::enable_shared_from_this<EchoSession> {
public:
    EchoSession(IOWorker& worker, int fd)
        : worker_(worker), fd_(fd) {}

    void bind(std::weak_ptr<EpollTcpConnectionWatcher> watcher_wp) {
        watcher_wp_ = std::move(watcher_wp);
    }

    void start() {
        LOG(INFO)("accepted fd=%d peer=%s", fd_, peerToString(fd_).c_str());
    }

    void onData(std::string_view data) {
        LOG(DEBUG)("fd=%d recv %zu bytes", fd_, data.size());
        LOG(INFO)("client payload:");
        LOG(INFO)("=== data begin ===\n%s", data.data());
        LOG(INFO)("=== data end ===");
        
        auto watcher = watcher_wp_.lock();
        if (!watcher) return;
        watcher->send(data);
    }

    void onClose() {
        LOG(INFO)("fd=%d closed", fd_);
    }

    void onError(int err) {
        LOG(WARN)("fd=%d error=%d (%s)", fd_, err, std::strerror(err));
    }

private:
    IOWorker& worker_;
    int fd_{-1};
    std::weak_ptr<EpollTcpConnectionWatcher> watcher_wp_;
};

int main() {
    logger::setMinLevel(LogLevel::INFO);

    constexpr int kPort = 8080;
    const int listen_fd = makeListenSocket(kPort);
    LOG(INFO)("tcp_server_example listening on 0.0.0.0:%d", kPort);

    Hive::Options opt;
    opt.io_backend = BackendType::LINUX_EPOLL;

    Hive hive(opt);
    auto worker = std::make_shared<IOWorker>(opt);
    hive.attachIOWorker(worker);

    hive.postToWorker(0, [worker, listen_fd]() {
        auto* self = IOWorker::currentWorker();
        if (!self) return;

        auto accept = std::make_shared<EpollTcpAcceptWatcher>(
            *worker,
            listen_fd,
            [worker](int client_fd) {
                auto* self = IOWorker::currentWorker();
                if (!self) return;

                auto watcher_wp_box =
                    std::make_shared<std::weak_ptr<EpollTcpConnectionWatcher>>();
                auto session = std::make_shared<EchoSession>(*worker, client_fd);

                auto on_data = [session](std::string_view data) { session->onData(data); };
                auto on_close = [session]() { session->onClose(); };
                auto on_error = [session](int err) { session->onError(err); };

                auto watcher = std::make_shared<EpollTcpConnectionWatcher>(
                    *worker,
                    client_fd,
                    std::move(on_data),
                    [watcher_wp_box, session]() {
                        session->onClose();
                        if (auto* w = IOWorker::currentWorker()) {
                            if (auto watcher = watcher_wp_box->lock()) {
                                w->releaseWatcher(watcher.get());
                            }
                        }
                    },
                    [watcher_wp_box, session](int err) {
                        session->onError(err);
                        if (auto* w = IOWorker::currentWorker()) {
                            if (auto watcher = watcher_wp_box->lock()) {
                                w->releaseWatcher(watcher.get());
                            }
                        }
                    });

                *watcher_wp_box = watcher;
                session->bind(*watcher_wp_box);
                session->start();

                self->adoptWatcher(watcher);
                watcher->start();
            });

        self->adoptWatcher(accept);
        accept->start();
        LOG(INFO)("accept watcher started");
    });

    hive.run();
    ::close(listen_fd);
    return 0;
}
