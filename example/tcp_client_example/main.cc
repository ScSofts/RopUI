#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <iostream>

#include <platform/schedule/hive.h>
#include <platform/schedule/io_worker.h>
#include <platform/linux/network/watcher/tcp_connect_watcher.h>
#include <platform/linux/network/watcher/tcp_connection_watcher.h>
#include <log.hpp>

using namespace RopHive;
using namespace RopHive::Linux;

/*
 * SimpleHttpClient
 *
 * - One-shot HTTP GET client
 * - Correct non-blocking connect state machine
 */
class SimpleHttpClient final : public std::enable_shared_from_this<SimpleHttpClient> {
public:
    using ResponseCallback = std::function<void(const std::string&)>;

    SimpleHttpClient(IOWorker& worker,
                     std::string host,
                     int port,
                     std::string path,
                     ResponseCallback cb)
        : worker_(worker),
          host_(std::move(host)),
          port_(port),
          path_(std::move(path)),
          cb_(std::move(cb)) {}

    void start() {
        auto self = shared_from_this();
        connect_ = std::make_shared<EpollTcpConnectWatcher>(
            worker_,
            host_,
            port_,
            [self](int connected_fd) { self->onConnected(connected_fd); },
            [self](int err) { self->finishWithError(err); });
        connect_->start();
    }

private:
    void onConnected(int connected_fd) {
        auto self = shared_from_this();
        conn_ = std::make_shared<EpollTcpConnectionWatcher>(
            worker_,
            connected_fd,
            [self](std::string_view chunk) { self->onData(chunk); },
            [self]() { self->finish(); },
            [self](int err) { self->finishWithError(err); });
        conn_->start();
        sendRequest();
    }

    void sendRequest() {
        std::string req =
            "GET " + path_ + " HTTP/1.1\r\n"
            "Host: " + host_ + "\r\n"
            "Connection: close\r\n\r\n";

        LOG(INFO)("send request:\n%s", req.c_str());
        if (conn_) {
            conn_->send(req);
            conn_->shutdownWrite();
        }
    }

    void onData(std::string_view chunk) {
        response_.append(chunk.data(), chunk.size());
    }

    void finish() {
        LOG(DEBUG)("finish()");
        if (done_) return;
        done_ = true;
        if (cb_) cb_(response_);
        connect_.reset();
        conn_.reset();
    }

    void finishWithError(int err) {
        LOG(WARN)("http client error: %d", err);
        finish();
    }

private:
    IOWorker& worker_;
    std::string host_;
    int port_;
    std::string path_;
    ResponseCallback cb_;

    bool done_{false};
    std::shared_ptr<EpollTcpConnectWatcher> connect_;
    std::shared_ptr<EpollTcpConnectionWatcher> conn_;
    std::string response_;
};

/* ---------------- demo main ---------------- */

int main() {
    logger::setMinLevel(LogLevel::DEBUG);

    Hive::Options opt;
    opt.io_backend = BackendType::LINUX_EPOLL;

    Hive hive(opt);
    auto worker = std::make_shared<IOWorker>(opt);
    hive.attachIOWorker(worker);

    hive.postToWorker(0, [worker, &hive]() {
        auto client = std::make_shared<SimpleHttpClient>(
            *worker,
            "127.0.0.1",
            8080,
            "/",
            [&](const std::string& resp) {
                std::cout << "=== response ===\n";
                std::cout << resp << "\n";
                hive.requestExit();
            });
        client->start();
    });

    hive.run();
    return 0;
}
