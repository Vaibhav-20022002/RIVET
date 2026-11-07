#pragma once

#include "../net/SocketUtils.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace rivet::core::ipc {

inline constexpr uint32_t kIpcMagic          = 0x52565449u; //< "RVTI"
inline constexpr uint32_t kMaxIpcPayloadSize = 65536u;

enum class IpcMsgType : uint16_t {
  WorkerReady           = 1,
  HandoffPublisher      = 2, //< SCM_RIGHTS client_fd + HandoffPayload (ip + buffered RTSP bytes)
  HandoffViewer         = 3, //< SCM_RIGHTS client_fd + HandoffPayload (ip + buffered RTSP bytes)
  PublisherStateChanged = 4, //< PublisherStatePayload (recording, active, generation)
  RequestMetrics        = 5,
  MetricsReply          = 6, //< MetricsSnapshot POD
  QuiesceAndDrain       = 7,
  Shutdown              = 8,
};

enum IpcHeaderFlags : uint16_t {
  kIpcFlagNone  = 0,
  kIpcFlagHasFd = 1u << 0,
};

struct IpcMsgHeader {
  uint32_t magic{kIpcMagic};
  uint16_t type{0};
  uint16_t flags{kIpcFlagNone};
  uint32_t payload_len{0};
  uint32_t reserved{0};
};
static_assert(sizeof(IpcMsgHeader) == 16, "IpcMsgHeader must be strictly 16 bytes");

struct WorkerReadyPayload {
  uint32_t pid{0};
  uint16_t video_rtp_port{0};
  uint16_t video_rtcp_port{0};
  uint16_t audio_rtp_port{0};
  uint16_t audio_rtcp_port{0};
};

struct PublisherStatePayload {
  uint8_t  has_publisher{0};
  uint8_t  is_recording{0};
  uint8_t  track_count{0};
  uint8_t  reserved{0};
  uint32_t config_generation{0};
};

/// Serializes `(client_ip, initial_bytes, auth_nonce)` for `HandoffPublisher` / `HandoffViewer`.
[[nodiscard]] inline std::vector<uint8_t> encode_handoff_payload(std::string_view client_ip,
    std::string_view                                                              initial_bytes,
    std::string_view                                                              auth_nonce = {}) {
  const uint16_t ip_len    = static_cast<uint16_t>(std::min<size_t>(client_ip.size(), 63));
  const uint16_t nonce_len = static_cast<uint16_t>(std::min<size_t>(auth_nonce.size(), 127));
  const uint32_t data_len  = static_cast<uint32_t>(
      std::min<size_t>(initial_bytes.size(), kMaxIpcPayloadSize - 10 - ip_len - nonce_len));
  std::vector<uint8_t> out(2 + ip_len + 4 + data_len + 2 + nonce_len);

  std::memcpy(out.data(), &ip_len, 2);
  if (ip_len > 0) {
    std::memcpy(out.data() + 2, client_ip.data(), ip_len);
  }
  std::memcpy(out.data() + 2 + ip_len, &data_len, 4);
  if (data_len > 0) {
    std::memcpy(out.data() + 2 + ip_len + 4, initial_bytes.data(), data_len);
  }
  const size_t nonce_off = 2 + ip_len + 4 + data_len;
  std::memcpy(out.data() + nonce_off, &nonce_len, 2);
  if (nonce_len > 0) {
    std::memcpy(out.data() + nonce_off + 2, auth_nonce.data(), nonce_len);
  }
  return out;
}

[[nodiscard]] inline bool decode_handoff_payload(const uint8_t *data,
    size_t                                                      len,
    std::string                                                &out_ip,
    std::string                                                &out_initial_bytes,
    std::string                                                *out_auth_nonce = nullptr) {
  if (len < 6) {
    return false;
  }
  uint16_t ip_len = 0;
  std::memcpy(&ip_len, data, 2);
  if (static_cast<size_t>(2 + ip_len + 4) > len) {
    return false;
  }
  out_ip.assign(reinterpret_cast<const char *>(data + 2), ip_len);

  uint32_t data_len = 0;
  std::memcpy(&data_len, data + 2 + ip_len, 4);
  const size_t base_end = static_cast<size_t>(2 + ip_len + 4 + data_len);
  if (base_end > len) {
    return false;
  }
  out_initial_bytes.assign(reinterpret_cast<const char *>(data + 2 + ip_len + 4), data_len);

  if (out_auth_nonce != nullptr) {
    out_auth_nonce->clear();
    if (base_end + 2 <= len) {
      uint16_t nonce_len = 0;
      std::memcpy(&nonce_len, data + base_end, 2);
      if (base_end + 2 + nonce_len <= len) {
        out_auth_nonce->assign(reinterpret_cast<const char *>(data + base_end + 2), nonce_len);
      }
    }
  }
  return true;
}

namespace detail {

[[nodiscard]] inline bool wait_fd_ready(int fd, short events, int timeout_ms = 2000) noexcept {
  struct pollfd pfd{};
  pfd.fd     = fd;
  pfd.events = events;
  while (true) {
    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc > 0) {
      return (pfd.revents & (POLLERR | POLLNVAL)) == 0;
    }
    if (rc < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
}

[[nodiscard]] inline bool write_exact(int fd, const uint8_t *buf, size_t len) noexcept {
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::send(fd, buf + off, len - off, net::nonblocking_send_flags());
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (!wait_fd_ready(fd, POLLOUT)) {
        return false;
      }
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] inline bool read_exact(int fd, uint8_t *buf, size_t len) noexcept {
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::recv(fd, buf + off, len - off, MSG_DONTWAIT);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      return false; //< EOF
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (!wait_fd_ready(fd, POLLIN)) {
        return false;
      }
      continue;
    }
    return false;
  }
  return true;
}

} //< namespace detail

/// Creates an `AF_UNIX` `SOCK_STREAM` socketpair configured with `O_NONBLOCK | FD_CLOEXEC`
/// and `128 KiB` socket buffers for zero-loss cold-path control + `SCM_RIGHTS` FD passing.
[[nodiscard]] inline bool create_uds_pair(int (&sv)[2]) noexcept {
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    sv[0] = -1;
    sv[1] = -1;
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    (void)net::set_nonblocking_cloexec(sv[i]);
    net::disable_sigpipe_on_socket(sv[i]);
    int buf = 128 * 1024;
    (void)::setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    (void)::setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  }
  return true;
}

/// Sends a framed IPC message over `uds_fd`, optionally passing `pass_fd` via `SCM_RIGHTS`
/// attached strictly to the 16-byte `IpcMsgHeader`.
[[nodiscard]] inline bool send_ipc_message(int uds_fd,
    IpcMsgType                                 type,
    const void                                *payload     = nullptr,
    uint32_t                                   payload_len = 0,
    int                                        pass_fd     = -1) noexcept {
  if (uds_fd < 0 || payload_len > kMaxIpcPayloadSize) {
    return false;
  }

  IpcMsgHeader hdr{};
  hdr.magic       = kIpcMagic;
  hdr.type        = static_cast<uint16_t>(type);
  hdr.flags       = (pass_fd >= 0) ? kIpcFlagHasFd : kIpcFlagNone;
  hdr.payload_len = payload_len;

  struct iovec iov{};
  iov.iov_base = &hdr;
  iov.iov_len  = sizeof(hdr);

  alignas(struct cmsghdr) char cmsg_buf[CMSG_SPACE(sizeof(int))]{};
  struct msghdr                msg{};
  msg.msg_iov    = &iov;
  msg.msg_iovlen = 1;

  if (pass_fd >= 0) {
    msg.msg_control    = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level     = SOL_SOCKET;
    cm->cmsg_type      = SCM_RIGHTS;
    cm->cmsg_len       = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cm), &pass_fd, sizeof(int));
  }

  ssize_t sent = -1;
  while (true) {
    sent = ::sendmsg(uds_fd, &msg, net::nonblocking_send_flags());
    if (sent > 0) {
      break;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (!detail::wait_fd_ready(uds_fd, POLLOUT)) {
        return false;
      }
      continue;
    }
    return false;
  }

  if (static_cast<size_t>(sent) < sizeof(hdr)) {
    const auto *rem_ptr = reinterpret_cast<const uint8_t *>(&hdr) + sent;
    if (!detail::write_exact(uds_fd, rem_ptr, sizeof(hdr) - static_cast<size_t>(sent))) {
      return false;
    }
  }

  if (payload_len > 0 && payload != nullptr) {
    if (!detail::write_exact(
            uds_fd, static_cast<const uint8_t *>(payload), static_cast<size_t>(payload_len))) {
      return false;
    }
  }
  return true;
}

enum class IpcRecvStatus : uint8_t {
  Ok,
  WouldBlock,
  EofOrError,
};

/// Non-blocking IPC message reader: reads strictly 16 bytes on the initial `recvmsg` with
/// `msg_control` armed so `SCM_RIGHTS` ancillary descriptors are never coalesced across messages.
[[nodiscard]] inline IpcRecvStatus recv_ipc_message(int uds_fd,
    IpcMsgHeader                                       &out_hdr,
    std::vector<uint8_t>                               &out_payload,
    int                                                &out_fd) {
  out_fd = -1;
  out_payload.clear();
  if (uds_fd < 0) {
    return IpcRecvStatus::EofOrError;
  }

  uint8_t      hdr_bytes[sizeof(IpcMsgHeader)]{};
  struct iovec iov{};
  iov.iov_base = hdr_bytes;
  iov.iov_len  = sizeof(hdr_bytes);

  alignas(struct cmsghdr) char cmsg_buf[CMSG_SPACE(sizeof(int))]{};
  struct msghdr                msg{};
  msg.msg_iov        = &iov;
  msg.msg_iovlen     = 1;
  msg.msg_control    = cmsg_buf;
  msg.msg_controllen = sizeof(cmsg_buf);

  ssize_t got = -1;
  while (true) {
    got = ::recvmsg(uds_fd, &msg, MSG_DONTWAIT);
    if (got > 0) {
      break;
    }
    if (got == 0) {
      return IpcRecvStatus::EofOrError;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return IpcRecvStatus::WouldBlock;
    }
    return IpcRecvStatus::EofOrError;
  }

  /// Extract any passed file descriptors from ancillary control data
  for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm != nullptr; cm = CMSG_NXTHDR(&msg, cm)) {
    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
      const size_t data_bytes = cm->cmsg_len - CMSG_LEN(0);
      const size_t fd_count   = data_bytes / sizeof(int);
      for (size_t i = 0; i < fd_count; ++i) {
        int rfd = -1;
        std::memcpy(&rfd, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
        if (rfd >= 0) {
          if (out_fd < 0) {
            (void)net::set_nonblocking_cloexec(rfd);
            net::disable_sigpipe_on_socket(rfd);
            out_fd = rfd;
          } else {
            ::close(rfd); //< Defensive close of unexpected extra FD
          }
        }
      }
    }
  }

  if (static_cast<size_t>(got) < sizeof(hdr_bytes)) {
    if (!detail::read_exact(
            uds_fd, hdr_bytes + got, sizeof(hdr_bytes) - static_cast<size_t>(got))) {
      if (out_fd >= 0) {
        ::close(out_fd);
        out_fd = -1;
      }
      return IpcRecvStatus::EofOrError;
    }
  }

  std::memcpy(&out_hdr, hdr_bytes, sizeof(out_hdr));
  if (out_hdr.magic != kIpcMagic || out_hdr.payload_len > kMaxIpcPayloadSize) {
    if (out_fd >= 0) {
      ::close(out_fd);
      out_fd = -1;
    }
    return IpcRecvStatus::EofOrError;
  }

  if ((out_hdr.flags & kIpcFlagHasFd) != 0 && out_fd < 0) {
    return IpcRecvStatus::EofOrError;
  }

  if (out_hdr.payload_len > 0) {
    out_payload.resize(out_hdr.payload_len);
    if (!detail::read_exact(uds_fd, out_payload.data(), out_hdr.payload_len)) {
      if (out_fd >= 0) {
        ::close(out_fd);
        out_fd = -1;
      }
      return IpcRecvStatus::EofOrError;
    }
  }

  return IpcRecvStatus::Ok;
}

} //< namespace rivet::core::ipc
