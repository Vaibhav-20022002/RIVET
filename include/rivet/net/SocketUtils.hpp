#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace rivet::net {

/// Sets `FD_CLOEXEC` on `fd`.
inline bool set_cloexec(int fd) noexcept {
  if (fd < 0) {
    return false;
  }
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

/// Clears `FD_CLOEXEC` on `fd` (used inside forked child right before `execve`).
inline bool clear_cloexec(int fd) noexcept {
  if (fd < 0) {
    return false;
  }
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == 0;
}

/// Sets `O_NONBLOCK` on `fd`.
inline bool set_nonblocking(int fd) noexcept {
  if (fd < 0) {
    return false;
  }
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/// Sets both `O_NONBLOCK` and `FD_CLOEXEC` on `fd`.
inline bool set_nonblocking_cloexec(int fd) noexcept {
  return set_nonblocking(fd) && set_cloexec(fd);
}

/// Suppresses `SIGPIPE` on a socket where supported (`SO_NOSIGPIPE` on Darwin/BSD).
inline void disable_sigpipe_on_socket(int fd) noexcept {
#if defined(SO_NOSIGPIPE)
  if (fd >= 0) {
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#else
  (void)fd;
#endif
}

/// Platform non-blocking send flags (`MSG_DONTWAIT | MSG_NOSIGNAL` on Linux, `MSG_DONTWAIT` on
/// macOS).
[[nodiscard]] constexpr int nonblocking_send_flags() noexcept {
#if defined(MSG_NOSIGNAL)
  return MSG_DONTWAIT | MSG_NOSIGNAL;
#else
  return MSG_DONTWAIT;
#endif
}

/// Configures a viewer TCP socket for low-latency non-blocking streaming.
inline void configure_tcp_viewer_socket(int fd) noexcept {
  if (fd < 0) {
    return;
  }
  (void)set_nonblocking_cloexec(fd);
  disable_sigpipe_on_socket(fd);

  int nodelay = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  /// Bounded 256 KiB send buffer prevents multi-second bufferbloat.
  int sndbuf = 256 * 1024;
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  int rcvbuf = 64 * 1024;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

#if defined(__linux__) && defined(TCP_NOTSENT_LOWAT)
  int lowat = 64 * 1024;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
#endif
}

/// Configures an upstream publisher TCP socket for non-blocking interleaved RTP ingest.
inline void configure_tcp_publisher_socket(int fd) noexcept {
  if (fd < 0) {
    return;
  }
  (void)set_nonblocking_cloexec(fd);
  disable_sigpipe_on_socket(fd);

  int nodelay = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  int rcvbuf = 2 * 1024 * 1024;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  int sndbuf = 128 * 1024;
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
}

/// Configures a UDP RTP/RTCP socket as non-blocking + cloexec with sized kernel buffers.
inline void
configure_udp_socket(int fd, int rcvbuf = 2 * 1024 * 1024, int sndbuf = 2 * 1024 * 1024) noexcept {
  if (fd < 0) {
    return;
  }
  (void)set_nonblocking_cloexec(fd);
  disable_sigpipe_on_socket(fd);

  int reuse = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (rcvbuf > 0) {
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }
  if (sndbuf > 0) {
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  }
}

/// Zero-DNS IPv4 sockaddr_in builder (`inet_pton` only — never blocks in DNS).
[[nodiscard]] inline bool
make_ipv4_addr(std::string_view ip, uint16_t port, sockaddr_in &out) noexcept {
  std::memset(&out, 0, sizeof(out));
#if defined(__APPLE__)
  out.sin_len = sizeof(out);
#endif
  out.sin_family = AF_INET;
  out.sin_port   = htons(port);

  if (ip.empty()) {
    ip = "127.0.0.1";
  }
  char ip_cstr[INET_ADDRSTRLEN]{};
  if (ip.size() >= sizeof(ip_cstr)) {
    return false;
  }
  std::memcpy(ip_cstr, ip.data(), ip.size());
  ip_cstr[ip.size()] = '\0';
  return ::inet_pton(AF_INET, ip_cstr, &out.sin_addr) == 1;
}

/// Binds a single non-blocking UDP socket on `port` (`0` = ephemeral) and populates `out_port`.
[[nodiscard]] inline int bind_udp_socket(uint16_t port,
    uint16_t                                     &out_port,
    int                                           rcvbuf = 2 * 1024 * 1024,
    int                                           sndbuf = 2 * 1024 * 1024) noexcept {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  configure_udp_socket(fd, rcvbuf, sndbuf);

  sockaddr_in addr{};
#if defined(__APPLE__)
  addr.sin_len = sizeof(addr);
#endif
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }

  sockaddr_in bound{};
  socklen_t   blen = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &blen) == 0) {
    out_port = ntohs(bound.sin_port);
  } else {
    out_port = port;
  }
  return fd;
}

/// Binds an RFC 3550 `(even RTP, odd RTCP = RTP + 1)` UDP socket pair.
/// If `desired_rtp_port == 0` or already in use when `allow_ephemeral_fallback` is true,
/// automatically probes for a free `(even, odd)` pair.
[[nodiscard]] inline bool bind_udp_port_pair(uint16_t desired_rtp_port,
    int                                              &out_rtp_fd,
    int                                              &out_rtcp_fd,
    uint16_t                                         &out_rtp_port,
    uint16_t                                         &out_rtcp_port,
    bool                                              allow_ephemeral_fallback = true) noexcept {
  out_rtp_fd    = -1;
  out_rtcp_fd   = -1;
  out_rtp_port  = 0;
  out_rtcp_port = 0;

  if (desired_rtp_port != 0 && (desired_rtp_port % 2) == 0 && desired_rtp_port < 65535) {
    uint16_t p_rtp  = 0;
    uint16_t p_rtcp = 0;
    int      fd1    = bind_udp_socket(desired_rtp_port, p_rtp);
    if (fd1 >= 0) {
      int fd2 = bind_udp_socket(static_cast<uint16_t>(p_rtp + 1), p_rtcp);
      if (fd2 >= 0) {
        out_rtp_fd    = fd1;
        out_rtcp_fd   = fd2;
        out_rtp_port  = p_rtp;
        out_rtcp_port = p_rtcp;
        return true;
      }
      ::close(fd1);
    }
    if (!allow_ephemeral_fallback) {
      return false;
    }
  }

  /// Probe for an available ephemeral (even, odd) UDP port pair
  for (int attempt = 0; attempt < 64; ++attempt) {
    uint16_t probe_port = 0;
    int      probe_fd   = bind_udp_socket(0, probe_port);
    if (probe_fd < 0) {
      return false;
    }

    if ((probe_port % 2) != 0) {
      ::close(probe_fd);
      if (probe_port >= 65534) {
        continue;
      }
      const uint16_t candidate_even = static_cast<uint16_t>(probe_port + 1);
      probe_fd                      = bind_udp_socket(candidate_even, probe_port);
      if (probe_fd < 0) {
        continue;
      }
    }

    if (probe_port == 0 || probe_port >= 65535) {
      ::close(probe_fd);
      continue;
    }

    uint16_t rtcp_p  = 0;
    int      rtcp_fd = bind_udp_socket(static_cast<uint16_t>(probe_port + 1), rtcp_p);
    if (rtcp_fd >= 0) {
      out_rtp_fd    = probe_fd;
      out_rtcp_fd   = rtcp_fd;
      out_rtp_port  = probe_port;
      out_rtcp_port = rtcp_p;
      return true;
    }
    ::close(probe_fd);
  }
  return false;
}

/// Resolves the absolute filesystem path of the currently executing binary for `fork` + `execve`.
[[nodiscard]] inline std::string resolve_self_executable() {
#if defined(__APPLE__)
  uint32_t size = 0;
  (void)::_NSGetExecutablePath(nullptr, &size);
  if (size > 0) {
    std::string buf(size, '\0');
    if (::_NSGetExecutablePath(buf.data(), &size) == 0) {
      buf.resize(std::strlen(buf.c_str()));
      return buf;
    }
  }
#elif defined(__linux__)
  char          buf[4096]{};
  const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len > 0) {
    buf[len] = '\0';
    return std::string(buf, static_cast<size_t>(len));
  }
#endif
  return {};
}

} //< namespace rivet::net
