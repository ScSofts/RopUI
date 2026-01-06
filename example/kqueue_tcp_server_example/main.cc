#ifdef __APPLE__

#include <log.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <memory>
#include <map>
#include <string>

#include <platform/schedule/eventloop.h>
#include <platform/macos/schedule/kqueue_backend.h>

using namespace RopHive;
using namespace RopHive::MacOS;

struct ClientContext {
    int socket;
    int client_id;
    ClientContext(int s, int id) : socket(s), client_id(id) {}
    ~ClientContext() { if (socket >= 0) close(socket); }
};

class TcpServer : public IWatcher,
                  public std::enable_shared_from_this<TcpServer> {
public:
    explicit TcpServer(EventLoop& loop, int port)
        : IWatcher(loop), port_(port), listen_socket_(-1) {}

    ~TcpServer() override {
        LOG(INFO)("TcpServer destructor called, cleaning up...");
        stop();
    }

    void start() override {
        LOG(INFO)("Starting TCP server initialization on port %d", port_);

        listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_socket_ < 0) {
            LOG(ERROR)("Failed to create listening socket, error: %s", strerror(errno));
            return;
        }
        LOG(INFO)("Created listening socket: %d", listen_socket_);

        // Set non-blocking
        int flags = fcntl(listen_socket_, F_GETFL, 0);
        fcntl(listen_socket_, F_SETFL, flags | O_NONBLOCK);

        int opt = 1;
        if (setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            LOG(WARN)("setsockopt SO_REUSEADDR failed: %s", strerror(errno));
        } else {
            LOG(INFO)("SO_REUSEADDR enabled");
        }

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listen_socket_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            LOG(ERROR)("Bind failed with error: %s", strerror(errno));
            close(listen_socket_);
            listen_socket_ = -1;
            return;
        }
        LOG(INFO)("Socket bound to port %d", port_);

        if (listen(listen_socket_, SOMAXCONN) < 0) {
            LOG(ERROR)("Listen failed with error: %s", strerror(errno));
            close(listen_socket_);
            listen_socket_ = -1;
            return;
        }
        LOG(INFO)("Server listening on port %d (backlog: %d)", port_, SOMAXCONN);

        // Bind listen socket to event loop
        LOG(INFO)("Binding listen socket to event loop");
        auto weak_self = weak_from_this();
        listen_source_ = std::make_shared<KqueueReadinessEventSource>(
            listen_socket_,
            [weak_self](const KqueueRawEvent& ev) {
                if (auto self = weak_self.lock()) {
                    self->handleAcceptEvent(ev);
                }
            });
        attachSource(listen_source_);

        LOG(INFO)("Kqueue TCP Server running on port %d", port_);
    }

    void stop() override {
        LOG(INFO)("Stopping TCP server...");

        if (listen_source_) {
            LOG(INFO)("Detaching listen event source");
            detachSource(listen_source_);
            listen_source_.reset();
        }
        if (listen_socket_ >= 0) {
            LOG(INFO)("Closing listen socket: %d", listen_socket_);
            close(listen_socket_);
            listen_socket_ = -1;
        }
        // Cleanup all clients
        LOG(INFO)("Cleaning up %zu connected clients", client_sources_.size());
        for (auto& pair : client_sources_) {
            detachSource(pair.second);
        }
        client_sources_.clear();
        clients_map_.clear();
        LOG(INFO)("Server stopped successfully");
    }

private:
    void handleAcceptEvent(const KqueueRawEvent& ev) {
        if (ev.ident != static_cast<uintptr_t>(listen_socket_)) return;

        sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_socket_, (sockaddr*)&client_addr, &addr_len);

        if (client_sock < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG(ERROR)("Accept failed with error: %s", strerror(errno));
            }
            return;
        }

        // Set non-blocking
        int flags = fcntl(client_sock, F_GETFL, 0);
        fcntl(client_sock, F_SETFL, flags | O_NONBLOCK);

        LOG(INFO)("New client connection accepted from %s:%d",
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        auto client = std::make_shared<ClientContext>(client_sock, next_id_++);
        clients_map_[client.get()] = client;

        LOG(INFO)("Client #%d connected, total clients: %zu", client->client_id, clients_map_.size());

        auto weak_self = weak_from_this();
        auto source = std::make_shared<KqueueReadinessEventSource>(
            client->socket,
            [weak_self, client](const KqueueRawEvent& e) {
                if (auto self = weak_self.lock()) {
                    self->handleClientEvent(client, e);
                }
            });

        attachSource(source);
        client_sources_[client.get()] = source;

        // Start receiving data
        postReceive(client);
    }

    void handleClientEvent(std::shared_ptr<ClientContext> client, const KqueueRawEvent& ev) {
        if (ev.ident != static_cast<uintptr_t>(client->socket)) return;

        if (ev.filter == EVFILT_READ) {
            handleClientRead(client);
        } else if (ev.filter == EVFILT_WRITE) {
            handleClientWrite(client);
        }
    }

    void handleClientRead(std::shared_ptr<ClientContext> client) {
        char buffer[1024];
        ssize_t n = recv(client->socket, buffer, sizeof(buffer), 0);

        if (n > 0) {
            std::string msg(buffer, n);
            LOG(INFO)("Received %ld bytes from client #%d: %s", n, client->client_id, msg.c_str());
            sendData(client, "Echo: " + msg);
        } else if (n == 0) {
            LOG(INFO)("Client #%d disconnected gracefully", client->client_id);
            removeClient(client.get());
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG(WARN)("Recv failed for client #%d with error: %s", client->client_id, strerror(errno));
                removeClient(client.get());
            }
        }
    }

    void handleClientWrite(std::shared_ptr<ClientContext> client) {
        // For simplicity, we don't handle partial writes in this example
        // In a real implementation, you'd need to buffer and handle partial sends
    }

    void postReceive(std::shared_ptr<ClientContext> client) {
        // Since we're using readiness-based, we just wait for the next EVFILT_READ event
        // The event source is already attached
    }

    void sendData(std::shared_ptr<ClientContext> client, const std::string& data) {
        LOG(INFO)("Sending %zu bytes to client #%d: %s", data.size(), client->client_id, data.c_str());

        ssize_t sent = send(client->socket, data.data(), data.size(), 0);
        if (sent < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG(WARN)("Send failed for client #%d with error: %s", client->client_id, strerror(errno));
                removeClient(client.get());
            } else {
                // Would need to buffer and wait for EVFILT_WRITE, but simplifying for this example
            }
        } else if (static_cast<size_t>(sent) < data.size()) {
            // Partial send - would need buffering, but simplifying
            LOG(WARN)("Partial send to client #%d, sent %ld/%zu", client->client_id, sent, data.size());
        }
    }

    void removeClient(ClientContext* ptr) {
        LOG(INFO)("Removing client #%d, remaining clients: %zu", ptr->client_id, client_sources_.size() - 1);

        if (client_sources_.count(ptr)) {
            detachSource(client_sources_[ptr]);
            client_sources_.erase(ptr);
        }
        clients_map_.erase(ptr);
    }

    int port_;
    int listen_socket_;
    std::map<ClientContext*, std::shared_ptr<ClientContext>> clients_map_;
    std::map<ClientContext*, std::shared_ptr<KqueueReadinessEventSource>> client_sources_;
    std::shared_ptr<KqueueReadinessEventSource> listen_source_;
    int next_id_ = 1;
};

int main() {
    LOG(INFO)("Starting Kqueue TCP Server Example");

    try {
        EventLoop loop(BackendType::MACOS_KQUEUE);
        auto server = std::make_shared<TcpServer>(loop, 8888);

        LOG(INFO)("Starting server...");
        server->start();

        LOG(INFO)("Event loop starting - server is now running");
        loop.run();

        LOG(INFO)("Event loop ended");
    } catch (const std::exception& e) {
        LOG(ERROR)("Exception caught in main: %s", e.what());
        return 1;
    } catch (...) {
        LOG(ERROR)("Unknown exception caught in main");
        return 1;
    }

    LOG(INFO)("Kqueue TCP Server Example finished");
    return 0;
}

#endif
