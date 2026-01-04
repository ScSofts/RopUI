#ifdef _WIN32
#include <log.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <string>
#include <memory>
#include <map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#include <platform/schedule/eventloop.h>
#include <platform/windows/schedule/iocp_backend.h>

using namespace RopHive;
using namespace RopHive::Windows;

struct IoContext {
    OVERLAPPED overlapped;
    enum { RECV, SEND, ACCEPT } type;
    char buffer[4096];
    WSABUF wsabuf;
    SOCKET accept_socket;

    IoContext() : accept_socket(INVALID_SOCKET) {
        memset(&overlapped, 0, sizeof(overlapped));
        wsabuf.buf = buffer;
        wsabuf.len = sizeof(buffer);
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

    // 确保析构时彻底清理
    ~TcpServer() { stop(); }

    void start() override {
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
        listen_socket_ = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        
        int opt = 1;
        setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in addr = { AF_INET, htons(port_), { INADDR_ANY } };
        bind(listen_socket_, (sockaddr*)&addr, sizeof(addr));
        listen(listen_socket_, SOMAXCONN);

        GUID guidAcceptEx = WSAID_ACCEPTEX;
        DWORD bytes;
        WSAIoctl(listen_socket_, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidAcceptEx, sizeof(guidAcceptEx),
                 &lpfnAcceptEx_, sizeof(lpfnAcceptEx_), &bytes, nullptr, nullptr);

        accept_source_ = std::make_unique<IocpHandleCompletionEventSource>(
            (HANDLE)listen_socket_, reinterpret_cast<ULONG_PTR>(this),
            [this](const IocpRawEvent& ev) { handleAcceptEvent(ev); });
        attachSource(accept_source_.get());

        postNewAccept();
        LOG(INFO)("Server started on port %d", port_);
    }

    // 完整的清理逻辑
    void stop() override {
        // 1. 停止监听
        if (accept_source_) {
            detachSource(accept_source_.get());
            accept_source_.reset();
        }
        if (listen_socket_ != INVALID_SOCKET) {
            closesocket(listen_socket_);
            listen_socket_ = INVALID_SOCKET;
        }

        // 2. 清理所有客户端连接及其关联的 Source
        // 先 detach 调度器中的 Source，再释放 client 内存
        for (auto& pair : client_sources_) {
            detachSource(pair.second.get());
        }
        client_sources_.clear();
        clients_map_.clear();

        WSACleanup();
        LOG(INFO)("Server stopped and cleaned up.");
    }

private:
    void postNewAccept() {
        SOCKET s = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        auto* io = new IoContext();
        io->type = IoContext::ACCEPT;
        io->accept_socket = s;

        DWORD recvd = 0;
        int addr_len = sizeof(sockaddr_in) + 16;
        // 关键：不读取首包数据(长度0)，彻底规避不sleep时的同步时序问题
        BOOL res = lpfnAcceptEx_(listen_socket_, s, io->buffer, 0, 
                                 addr_len, addr_len, &recvd, &io->overlapped);
        
        if (!res && WSAGetLastError() != ERROR_IO_PENDING) {
            closesocket(s);
            delete io;
        }
    }

    void handleAcceptEvent(const IocpRawEvent& ev) {
        auto* accept_io = CONTAINING_RECORD(ev.overlapped, IoContext, overlapped);
        SOCKET client_sock = accept_io->accept_socket;
        delete accept_io;

        if (ev.error != 0) {
            if (client_sock != INVALID_SOCKET) closesocket(client_sock);
            postNewAccept();
            return;
        }

        postNewAccept();
        setsockopt(client_sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&listen_socket_, sizeof(listen_socket_));

        auto client = std::make_shared<ClientContext>(client_sock, next_id_++);
        clients_map_[client.get()] = client;

        auto source = std::make_unique<IocpHandleCompletionEventSource>(
            (HANDLE)client->socket, reinterpret_cast<ULONG_PTR>(client.get()),
            [this, client](const IocpRawEvent& e) { handleClientEvent(client, e); });
        
        auto* source_ptr = source.get();
        client_sources_[client.get()] = std::move(source);
        attachSource(source_ptr);

        // 既然不 sleep 会卡，说明数据已经到了，这里 postReceive 会立即触发下一次 handleClientEvent
        postReceive(client);
    }

    void handleClientEvent(std::shared_ptr<ClientContext> client, const IocpRawEvent& ev) {
        auto* io = CONTAINING_RECORD(ev.overlapped, IoContext, overlapped);
        
        if (ev.error != 0 || ev.bytes == 0) {
            delete io;
            removeClient(client.get());
            return;
        }

        if (io->type == IoContext::RECV) {
            std::string msg(io->buffer, ev.bytes);
            LOG(INFO)("Recv #%d: %s", client->client_id, msg.c_str());
            sendData(client, "Echo: " + msg);
            delete io;
            postReceive(client);
        } else {
            delete io;
        }
    }

    void postReceive(std::shared_ptr<ClientContext> client) {
        auto* io = new IoContext();
        io->type = IoContext::RECV;
        DWORD flags = 0, bytes = 0;
        if (WSARecv(client->socket, &io->wsabuf, 1, &bytes, &flags, &io->overlapped, nullptr) == SOCKET_ERROR) {
            if (WSAGetLastError() != WSA_IO_PENDING) {
                delete io;
                removeClient(client.get());
            }
        }
    }

    void sendData(std::shared_ptr<ClientContext> client, const std::string& data) {
        auto* io = new IoContext();
        io->type = IoContext::SEND;
        size_t len = (std::min)((size_t)data.size(), sizeof(io->buffer));
        memcpy(io->buffer, data.data(), len);
        io->wsabuf.len = (ULONG)len;
        
        if (WSASend(client->socket, &io->wsabuf, 1, nullptr, 0, &io->overlapped, nullptr) == SOCKET_ERROR) {
            if (WSAGetLastError() != WSA_IO_PENDING) delete io;
        }
    }

    void removeClient(ClientContext* ptr) {
        auto it = client_sources_.find(ptr);
        if (it != client_sources_.end()) {
            detachSource(it->second.get());
            client_sources_.erase(it);
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
    EventLoop loop(BackendType::WINDOWS_IOCP);
    TcpServer server(loop, 8888);
    server.start();
    loop.run();
    return 0;
}
#endif