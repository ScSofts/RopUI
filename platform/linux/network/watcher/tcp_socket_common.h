#ifndef _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_SOCKET_COMMON_H
#define _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_SOCKET_COMMON_H

#ifdef __linux__

#include <cerrno>
#include <cstring>
#include <optional>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../../network/watcher/tcp_watchers.h"

namespace RopHive::Linux::TcpDetail {

inline void closeFd(int &fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

inline bool setNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return false;
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    return false;
  return true;
}

inline void applyCloseOnExecIfConfigured(int fd, const std::optional<bool> &v) {
  if (!v.has_value())
    return;
  int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0)
    return;
  if (*v) {
    flags |= FD_CLOEXEC;
  } else {
    flags &= ~FD_CLOEXEC;
  }
  ::fcntl(fd, F_SETFD, flags);
}

inline void applyTcpNoDelayIfConfigured(int fd, const std::optional<bool> &v) {
  if (!v.has_value())
    return;
  const int yes = *v ? 1 : 0;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

inline void applyKeepAliveIfConfigured(int fd,
                                       const std::optional<bool> &enable,
                                       const std::optional<int> &idle_sec,
                                       const std::optional<int> &interval_sec,
                                       const std::optional<int> &count) {
  if (!enable.has_value())
    return;
  const int yes = *enable ? 1 : 0;
  ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
  if (!*enable)
    return;

#ifdef TCP_KEEPIDLE
  if (idle_sec.has_value()) {
    const int v = *idle_sec;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof(v));
  }
#endif
#ifdef TCP_KEEPINTVL
  if (interval_sec.has_value()) {
    const int v = *interval_sec;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v));
  }
#endif
#ifdef TCP_KEEPCNT
  if (count.has_value()) {
    const int v = *count;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof(v));
  }
#endif
}

inline void applyBufSizeIfConfigured(int fd, const std::optional<int> &rcv,
                                     const std::optional<int> &snd) {
  if (rcv.has_value()) {
    const int v = *rcv;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
  }
  if (snd.has_value()) {
    const int v = *snd;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
  }
}

inline void applyLingerIfConfigured(int fd,
                                    const std::optional<int> &linger_sec) {
  if (!linger_sec.has_value())
    return;
  ::linger lin{};
  if (*linger_sec < 0) {
    lin.l_onoff = 0;
    lin.l_linger = 0;
  } else {
    lin.l_onoff = 1;
    lin.l_linger = *linger_sec;
  }
  ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
}

inline void applyReuseAddrIfConfigured(int fd, const std::optional<bool> &v) {
  if (!v.has_value())
    return;
  const int yes = *v ? 1 : 0;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
}

#ifdef SO_REUSEPORT
inline void applyReusePortIfConfigured(int fd, const std::optional<bool> &v) {
  if (!v.has_value())
    return;
  const int yes = *v ? 1 : 0;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
}
#else
inline void applyReusePortIfConfigured(int, const std::optional<bool> &) {}
#endif

inline void applyIpv6OnlyIfConfigured(int fd, const std::optional<bool> &v) {
  if (!v.has_value())
    return;
  const int yes = *v ? 1 : 0;
  ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
}

inline bool toSockaddr(const RopHive::Network::IpEndpoint &ep,
                       sockaddr_storage &out, socklen_t &out_len) {
  std::memset(&out, 0, sizeof(out));

  if (const auto *v4 = std::get_if<RopHive::Network::IpEndpointV4>(&ep)) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(v4->port);
    std::memcpy(&a.sin_addr, v4->ip.data(), v4->ip.size());
    std::memcpy(&out, &a, sizeof(a));
    out_len = sizeof(a);
    return true;
  }

  if (const auto *v6 = std::get_if<RopHive::Network::IpEndpointV6>(&ep)) {
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(v6->port);
    std::memcpy(&a.sin6_addr, v6->ip.data(), v6->ip.size());
    a.sin6_scope_id = v6->scope_id;
    std::memcpy(&out, &a, sizeof(a));
    out_len = sizeof(a);
    return true;
  }

  return false;
}

inline std::optional<RopHive::Network::IpEndpoint>
fromSockaddr(const sockaddr *sa, socklen_t len) {
  if (!sa)
    return std::nullopt;

  if (sa->sa_family == AF_INET) {
    if (len < static_cast<socklen_t>(sizeof(sockaddr_in)))
      return std::nullopt;
    const auto *a = reinterpret_cast<const sockaddr_in *>(sa);
    RopHive::Network::IpEndpointV4 v4;
    std::memcpy(v4.ip.data(), &a->sin_addr, v4.ip.size());
    v4.port = ntohs(a->sin_port);
    return RopHive::Network::IpEndpoint{v4};
  }

  if (sa->sa_family == AF_INET6) {
    if (len < static_cast<socklen_t>(sizeof(sockaddr_in6)))
      return std::nullopt;
    const auto *a = reinterpret_cast<const sockaddr_in6 *>(sa);
    RopHive::Network::IpEndpointV6 v6;
    std::memcpy(v6.ip.data(), &a->sin6_addr, v6.ip.size());
    v6.port = ntohs(a->sin6_port);
    v6.scope_id = a->sin6_scope_id;
    return RopHive::Network::IpEndpoint{v6};
  }

  return std::nullopt;
}

inline void bestEffortFillEndpoints(int fd,
                                    RopHive::Network::ITcpStream &stream) {
  sockaddr_storage peer{};
  socklen_t peer_len = sizeof(peer);
  if (!stream.peer.has_value() && ::getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &peer_len) == 0) {
    stream.peer = fromSockaddr(reinterpret_cast<sockaddr *>(&peer), peer_len);
  }

  sockaddr_storage local{};
  socklen_t local_len = sizeof(local);
  if (!stream.local.has_value() && ::getsockname(fd, reinterpret_cast<sockaddr *>(&local), &local_len) ==
      0) {
    stream.local =
        fromSockaddr(reinterpret_cast<sockaddr *>(&local), local_len);
  }
}

} // namespace RopHive::Linux::TcpDetail

namespace RopHive::Linux {

class LinuxTcpStream final : public RopHive::Network::ITcpStream {
public:
  LinuxTcpStream(RopHive::Network::TcpStreamKind kind, int fd)
      : ITcpStream(kind), fd_(fd) {}

  ~LinuxTcpStream() override { TcpDetail::closeFd(fd_); }

  int fd() const noexcept { return fd_; }

  int releaseFd() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

private:
  int fd_{-1};
};

} // namespace RopHive::Linux

#endif // __linux__

#endif // _ROP_PLATFORM_LINUX_NETWORK_WATCHER_TCP_SOCKET_COMMON_H
