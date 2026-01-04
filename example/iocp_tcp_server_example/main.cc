#ifdef _WIN32
#include <log.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <string>
#include <memory>
#include <map>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

#include <platform/schedule/eventloop.h>
#include <platform/windows/schedule/iocp_backend.h>

using namespace RopHive;
using namespace RopHive::Windows;

// 1. 严格定义 IoContext，首包模式对 buffer 布局有硬性要求
struct IoContext {
    OVERLAPPED overlapped;
    enum { RECV, SEND, ACCEPT } type;
    
    // 缓冲区布局：[数据区 (1024)] + [本地地址 (32)] + [远程地址 (32)]
    static constexpr int DATA_LEN = 1024;
    static constexpr int ADDR_LEN = sizeof(sockaddr_in) + 16;
    char buffer[DATA_LEN + ADDR_LEN * 2];
    
    WSABUF wsabuf;
    SOCKET accept_socket;

    IoContext() : accept_socket(INVALID_SOCKET) {
        memset(&overlapped, 0, sizeof(overlapped));
        wsabuf.buf = buffer;
        wsabuf.len = DATA_LEN;
    }
};

struct ClientContext {
    SOCKET socket;
    int client_id;
    ClientContext(SOCKET s, int id) : socket(s), client_id(id) {}
    ~ClientContext() { if (socket != INVALID_SOCKET) closesocket(socket); }
};

class TcpServer : public IWatcher {
public:
    explicit TcpServer(EventLoop& loop, int port)
        : IWatcher(loop), port_(port), listen_socket_(INVALID_SOCKET) {}

    ~TcpServer() { 
        LOG(INFO)("TcpServer destructor called, cleaning up...");
        stop(); 
    }

    void start() override {
        LOG(INFO)("Starting TCP server initialization on port %d", port_);
        
        WSADATA wsa; 
        int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (wsa_result != 0) {
            LOG(ERROR)("WSAStartup failed with error: %d", wsa_result);
            return;
        }
        LOG(INFO)("WSAStartup successful, version %d.%d", LOBYTE(wsa.wVersion), HIBYTE(wsa.wVersion));
        
        listen_socket_ = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (listen_socket_ == INVALID_SOCKET) {
            LOG(ERROR)("Failed to create listening socket, error: %d", WSAGetLastError());
            return;
        }
        LOG(INFO)("Created listening socket: %d", listen_socket_);
        
        int opt = 1;
        if (setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
            LOG(WARN)("setsockopt SO_REUSEADDR failed: %d", WSAGetLastError());
        } else {
            LOG(INFO)("SO_REUSEADDR enabled");
        }

        sockaddr_in addr = { AF_INET, htons(port_), { INADDR_ANY } };
        if (bind(listen_socket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            LOG(ERROR)("Bind failed with error: %d", WSAGetLastError());
            closesocket(listen_socket_);
            listen_socket_ = INVALID_SOCKET;
            return;
        }
        LOG(INFO)("Socket bound to port %d", port_);
        
        if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
            LOG(ERROR)("Listen failed with error: %d", WSAGetLastError());
            closesocket(listen_socket_);
            listen_socket_ = INVALID_SOCKET;
            return;
        }
        LOG(INFO)("Server listening on port %d (backlog: %d)", port_, SOMAXCONN);

        GUID guidAcceptEx = WSAID_ACCEPTEX;
        DWORD bytes;
        if (WSAIoctl(listen_socket_, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidAcceptEx, sizeof(guidAcceptEx),
                     &lpfnAcceptEx_, sizeof(lpfnAcceptEx_), &bytes, nullptr, nullptr) == SOCKET_ERROR) {
            LOG(ERROR)("Failed to get AcceptEx function pointer: %d", WSAGetLastError());
            return;
        }
        LOG(INFO)("AcceptEx function pointer obtained successfully");

        // 绑定监听 Socket 到调度器
        LOG(INFO)("Binding listen socket to event loop");
        accept_source_ = std::make_unique<IocpHandleCompletionEventSource>(
            (HANDLE)listen_socket_, reinterpret_cast<ULONG_PTR>(this),
            [this](const IocpRawEvent& ev) { handleAcceptEvent(ev); });
        attachSource(accept_source_.get());

        LOG(INFO)("Starting initial accept request");
        postNewAccept();
        LOG(INFO)("Industrial IOCP Server (Initial Data Mode) running on port %d", port_);
    }

    void stop() override {
        LOG(INFO)("Stopping TCP server...");
        
        if (accept_source_) {
            LOG(INFO)("Detaching accept event source");
            detachSource(accept_source_.get());
            accept_source_.reset();
        }
        if (listen_socket_ != INVALID_SOCKET) {
            LOG(INFO)("Closing listen socket: %d", listen_socket_);
            closesocket(listen_socket_);
            listen_socket_ = INVALID_SOCKET;
        }
        // 清理所有客户端
        LOG(INFO)("Cleaning up %zu connected clients", client_sources_.size());
        for (auto& pair : client_sources_) {
            detachSource(pair.second.get());
        }
        client_sources_.clear();
        clients_map_.clear();
        WSACleanup();
        LOG(INFO)("Server stopped successfully");
    }

private:
    void postNewAccept() {
        SOCKET s = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (s == INVALID_SOCKET) {
            LOG(ERROR)("Failed to create accept socket: %d", WSAGetLastError());
            return;
        }
        
        auto* io = new IoContext();
        io->type = IoContext::ACCEPT;
        io->accept_socket = s;

        DWORD recvd = 0;
        // 关键：dwReceiveDataLength = 1024 (DATA_LEN)
        // 只有当至少 1 个字节的数据到达时，IOCP 才会抛出完成事件
        BOOL res = lpfnAcceptEx_(listen_socket_, s, io->buffer, 
                                 IoContext::DATA_LEN, IoContext::ADDR_LEN, IoContext::ADDR_LEN,
                                 &recvd, &io->overlapped);
        
        if (!res && WSAGetLastError() != ERROR_IO_PENDING) {
            LOG(ERROR)("AcceptEx failed with error: %d", WSAGetLastError());
            closesocket(s);
            delete io;
        }
        // 如果成功或ERROR_IO_PENDING，则等待完成端口通知
    }

    void handleAcceptEvent(const IocpRawEvent& ev) {
        auto* accept_io = CONTAINING_RECORD(ev.overlapped, IoContext, overlapped);
        SOCKET client_sock = accept_io->accept_socket;

        if (ev.error != 0) {
            LOG(ERROR)("Accept event failed with error: %d", ev.error);
            if (client_sock != INVALID_SOCKET) closesocket(client_sock);
            delete accept_io;
            postNewAccept();
            return;
        }

        // 1. 此时连接已建立，且内核已经帮我们读好了 ev.bytes 字节的数据
        std::string initial_data(accept_io->buffer, ev.bytes);
        delete accept_io;

        LOG(INFO)("New client connection accepted, initial data length: %lu bytes", ev.bytes);

        // 2. 立即拉起下一个监听
        postNewAccept();

        // 3. 继承监听 Socket 的属性
        setsockopt(client_sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&listen_socket_, sizeof(listen_socket_));

        auto client = std::make_shared<ClientContext>(client_sock, next_id_++);
        clients_map_[client.get()] = client;

        LOG(INFO)("Client #%d connected, total clients: %zu", client->client_id, clients_map_.size());

        auto source = std::make_unique<IocpHandleCompletionEventSource>(
            (HANDLE)client->socket, reinterpret_cast<ULONG_PTR>(client.get()),
            [this, client](const IocpRawEvent& e) { handleClientEvent(client, e); });
        
        auto* source_ptr = source.get();
        client_sources_[client.get()] = std::move(source);
        attachSource(source_ptr);

        // 4. 处理随 Accept 一起送达的首包数据
        if (!initial_data.empty()) {
            LOG(INFO)("Initial Data Caught from client #%d (Length: %zu): %s", 
                     client->client_id, initial_data.size(), initial_data.c_str());
            sendData(client, "Echo(Initial): " + initial_data);
        }

        // 5. 开启正常的后续读取流程
        postReceive(client);
    }

    void handleClientEvent(std::shared_ptr<ClientContext> client, const IocpRawEvent& ev) {
        auto* io = CONTAINING_RECORD(ev.overlapped, IoContext, overlapped);
        
        if (ev.error != 0 || ev.bytes == 0) {
            if (ev.error != 0) {
                LOG(WARN)("Client #%d connection error: %d", client->client_id, ev.error);
            } else {
                LOG(INFO)("Client #%d disconnected gracefully", client->client_id);
            }
            delete io;
            removeClient(client.get());
            return;
        }

        if (io->type == IoContext::RECV) {
            std::string msg(io->buffer, ev.bytes);
            LOG(INFO)("Received %lu bytes from client #%d: %s", ev.bytes, client->client_id, msg.c_str());
            sendData(client, "Echo: " + msg);
            delete io;
            postReceive(client);
        } else if (io->type == IoContext::SEND) {
            if (ev.error == 0) {
                LOG(INFO)("Successfully sent %lu bytes to client #%d", ev.bytes, client->client_id);
            } else {
                LOG(WARN)("Send failed for client #%d with error: %d", client->client_id, ev.error);
            }
            delete io;
        } else {
            LOG(WARN)("Unknown IoContext type: %d", io->type);
            delete io;
        }
    }

    void postReceive(std::shared_ptr<ClientContext> client) {
        auto* io = new IoContext();
        io->type = IoContext::RECV;
        DWORD flags = 0, bytes = 0;
        int result = WSARecv(client->socket, &io->wsabuf, 1, &bytes, &flags, &io->overlapped, nullptr);
        if (result == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                LOG(WARN)("WSARecv failed for client #%d with error: %d", client->client_id, error);
                delete io;
                removeClient(client.get());
            } else {
                // I/O pending, normal operation
            }
        }
    }

    void sendData(std::shared_ptr<ClientContext> client, const std::string& data) {
        auto* io = new IoContext();
        io->type = IoContext::SEND;
        size_t len = (std::min)((size_t)data.size(), (size_t)IoContext::DATA_LEN);
        memcpy(io->buffer, data.data(), len);
        io->wsabuf.len = (ULONG)len;
        
        LOG(INFO)("Sending %zu bytes to client #%d: %s", len, client->client_id, data.c_str());
        
        int result = WSASend(client->socket, &io->wsabuf, 1, nullptr, 0, &io->overlapped, nullptr);
        if (result == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                LOG(WARN)("WSASend failed for client #%d with error: %d", client->client_id, error);
                delete io;
            }
            // If WSA_IO_PENDING, the completion will be handled in handleClientEvent
        }
    }

    void removeClient(ClientContext* ptr) {
        LOG(INFO)("Removing client #%d, remaining clients: %zu", ptr->client_id, client_sources_.size() - 1);
        
        if (client_sources_.count(ptr)) {
            detachSource(client_sources_[ptr].get());
            client_sources_.erase(ptr);
        }
        clients_map_.erase(ptr);
    }

    int port_;
    SOCKET listen_socket_;
    LPFN_ACCEPTEX lpfnAcceptEx_ = nullptr;
    std::map<ClientContext*, std::shared_ptr<ClientContext>> clients_map_;
    std::map<ClientContext*, std::unique_ptr<IocpHandleCompletionEventSource>> client_sources_;
    std::unique_ptr<IocpHandleCompletionEventSource> accept_source_;
    int next_id_ = 1;
};

int main() {
    LOG(INFO)("Starting IOCP TCP Server Example");
    
    try {
        EventLoop loop(BackendType::WINDOWS_IOCP);
        TcpServer server(loop, 8888);
        
        LOG(INFO)("Starting server...");
        server.start();
        
        LOG(INFO)("Event loop starting - server is now running");
        // 调度器开始工作
        loop.run();
        
        LOG(INFO)("Event loop ended");
    } catch (const std::exception& e) {
        LOG(ERROR)("Exception caught in main: %s", e.what());
        return 1;
    } catch (...) {
        LOG(ERROR)("Unknown exception caught in main");
        return 1;
    }
    
    LOG(INFO)("IOCP TCP Server Example finished");
    return 0;
}
#endif
