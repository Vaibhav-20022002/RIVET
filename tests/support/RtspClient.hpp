#pragma once

#include "rivet/config/RivetConfig.hpp"
#include "rivet/net/SocketUtils.hpp"
#include "rivet/proto/rtsp/RtspDigestAuth.hpp"
#include "rivet/proto/rtsp/RtspStreamFramer.hpp"
#include "tests/support/Eventually.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rivet::test {

struct InterleavedPacket {
  uint8_t              channel{0};
  std::vector<uint8_t> payload;
};

struct RtspClientResponse {
  int         status_code{0};
  std::string raw_text;
  std::string headers;
  std::string body;
  size_t      interleaved_frames_before_reply{0};
};

/// Timeout-safe RTSP 1.0 publisher & viewer test client (TCP interleaved `$` + dedicated UDP).
class RtspTestClient {
  int                           fd_{-1};
  int                           udp_rtp_fd_{-1};
  int                           udp_rtcp_fd_{-1};
  uint16_t                      client_rtp_port_{0};
  uint16_t                      client_rtcp_port_{0};
  uint16_t                      server_rtp_port_{0};
  uint16_t                      server_rtcp_port_{0};
  uint32_t                      cseq_{1};
  std::string                   session_id_;
  proto::rtsp::RtspReadBuffer   read_buf_{256 * 1024};
  std::deque<InterleavedPacket> interleaved_inbox_;

  static bool write_all(int fd, const void *buf, size_t len, int timeout_ms = 2000) {
    const auto *p      = static_cast<const uint8_t *>(buf);
    size_t      off    = 0;
    const int   scaled = timeout_ms * timeout_scale_factor();
    while (off < len) {
      struct pollfd pfd{};
      pfd.fd       = fd;
      pfd.events   = POLLOUT;
      const int pr = ::poll(&pfd, 1, scaled);
      if (pr <= 0) {
        return false;
      }
      const ssize_t n = ::send(fd, p + off, len - off, net::nonblocking_send_flags());
      if (n > 0) {
        off += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      return false;
    }
    return true;
  }

public:
  RtspTestClient() = default;
  ~RtspTestClient() { close_all(); }

  RtspTestClient(const RtspTestClient &)            = delete;
  RtspTestClient &operator=(const RtspTestClient &) = delete;

  bool connect(uint16_t port, std::string_view host = "127.0.0.1") {
    close_tcp();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }
    net::disable_sigpipe_on_socket(fd_);
    int nodelay = 1;
    (void)::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    sockaddr_in addr{};
    if (!net::make_ipv4_addr(host, port, addr)) {
      close_tcp();
      return false;
    }
    if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      close_tcp();
      return false;
    }
    (void)net::set_nonblocking_cloexec(fd_);
    read_buf_.clear();
    interleaved_inbox_.clear();
    return true;
  }

  void close_tcp() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void close_all() noexcept {
    close_tcp();
    if (udp_rtp_fd_ >= 0) {
      ::close(udp_rtp_fd_);
      udp_rtp_fd_ = -1;
    }
    if (udp_rtcp_fd_ >= 0) {
      ::close(udp_rtcp_fd_);
      udp_rtcp_fd_ = -1;
    }
  }

  [[nodiscard]] int                fd() const noexcept { return fd_; }
  [[nodiscard]] int                udp_rtp_fd() const noexcept { return udp_rtp_fd_; }
  [[nodiscard]] int                udp_rtcp_fd() const noexcept { return udp_rtcp_fd_; }
  [[nodiscard]] uint16_t           client_rtp_port() const noexcept { return client_rtp_port_; }
  [[nodiscard]] uint16_t           client_rtcp_port() const noexcept { return client_rtcp_port_; }
  [[nodiscard]] uint16_t           server_rtp_port() const noexcept { return server_rtp_port_; }
  [[nodiscard]] uint16_t           server_rtcp_port() const noexcept { return server_rtcp_port_; }
  [[nodiscard]] const std::string &session_id() const noexcept { return session_id_; }
  void                             set_session_id(std::string s) { session_id_ = std::move(s); }

  bool bind_udp_ports() {
    if (udp_rtp_fd_ >= 0) {
      return true;
    }
    return net::bind_udp_port_pair(
        0, udp_rtp_fd_, udp_rtcp_fd_, client_rtp_port_, client_rtcp_port_, false);
  }

  bool send_raw(std::string_view raw, int timeout_ms = 2000) {
    if (fd_ < 0) return false;
    return write_all(fd_, raw.data(), raw.size(), timeout_ms);
  }

  bool
  send_interleaved(uint8_t channel, const std::vector<uint8_t> &payload, int timeout_ms = 2000) {
    return send_interleaved(channel, payload.data(), payload.size(), timeout_ms);
  }

  bool send_interleaved(uint8_t channel, const uint8_t *data, size_t len, int timeout_ms = 2000) {
    if (fd_ < 0 || len > 65535U) return false;
    uint8_t hdr[4] = {static_cast<uint8_t>('$'),
        channel,
        static_cast<uint8_t>((len >> 8U) & 0xFFU),
        static_cast<uint8_t>(len & 0xFFU)};
    if (!write_all(fd_, hdr, 4, timeout_ms)) return false;
    return (len == 0) || write_all(fd_, data, len, timeout_ms);
  }

  bool send_udp_rtp(const std::vector<uint8_t> &pkt, uint16_t dst_port = 0) {
    if (udp_rtp_fd_ < 0 && !bind_udp_ports()) {
      return false;
    }
    const uint16_t port = (dst_port != 0) ? dst_port : server_rtp_port_;
    sockaddr_in    dst{};
    if (!net::make_ipv4_addr("127.0.0.1", port, dst)) {
      return false;
    }
    const ssize_t n = ::sendto(udp_rtp_fd_,
        pkt.data(),
        pkt.size(),
        net::nonblocking_send_flags(),
        reinterpret_cast<const sockaddr *>(&dst),
        sizeof(dst));
    return n == static_cast<ssize_t>(pkt.size());
  }

  bool send_udp_rtcp(const uint8_t *data, size_t len, uint16_t dst_port = 0) {
    if (udp_rtcp_fd_ < 0 && !bind_udp_ports()) {
      return false;
    }
    const uint16_t port = (dst_port != 0) ? dst_port : server_rtcp_port_;
    sockaddr_in    dst{};
    if (!net::make_ipv4_addr("127.0.0.1", port, dst)) {
      return false;
    }
    const ssize_t n = ::sendto(udp_rtcp_fd_,
        data,
        len,
        net::nonblocking_send_flags(),
        reinterpret_cast<const sockaddr *>(&dst),
        sizeof(dst));
    return n == static_cast<ssize_t>(len);
  }

  /// Sends an RTSP request and reads until the RTSP response arrives (recording any `$` frames
  /// that arrived on the wire BEFORE the RTSP response so reply ordering is testable!).
  RtspClientResponse request(std::string_view method,
      std::string_view                        url,
      std::string_view                        extra_headers = {},
      std::string_view                        body          = {},
      int                                     timeout_ms    = 2500) {
    RtspClientResponse out{};
    if (fd_ < 0) {
      return out;
    }

    std::string req;
    req.reserve(256 + extra_headers.size() + body.size());
    req.append(method);
    req.push_back(' ');
    req.append(url);
    req.append(" RTSP/1.0\r\nCSeq: ");
    req.append(std::to_string(cseq_++));
    req.append("\r\n");
    if (!session_id_.empty() && extra_headers.find("Session:") == std::string_view::npos) {
      req.append("Session: ");
      req.append(session_id_);
      req.append("\r\n");
    }
    if (!extra_headers.empty()) {
      req.append(extra_headers);
      if (!req.ends_with("\r\n")) {
        req.append("\r\n");
      }
    }
    if (!body.empty()) {
      if (extra_headers.find("Content-Type:") == std::string_view::npos) {
        req.append("Content-Type: application/sdp\r\n");
      }
      req.append("Content-Length: ");
      req.append(std::to_string(body.size()));
      req.append("\r\n");
    }
    req.append("\r\n");
    req.append(body);

    if (!write_all(fd_, req.data(), req.size(), timeout_ms)) {
      return out;
    }

    return recv_rtsp_response(timeout_ms);
  }

  RtspClientResponse recv_rtsp_response(int timeout_ms = 2500) {
    RtspClientResponse out{};
    if (fd_ < 0) {
      return out;
    }
    const size_t initial_interleaved = interleaved_inbox_.size();
    const auto   deadline            = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms * timeout_scale_factor());

    while (std::chrono::steady_clock::now() < deadline) {
      while (!read_buf_.empty()) {
        auto view = read_buf_.next_view();
        if (view.type == proto::rtsp::FramedMessageType::None) {
          break;
        }
        if (view.type == proto::rtsp::FramedMessageType::Error) {
          return out;
        }
        if (view.type == proto::rtsp::FramedMessageType::Interleaved) {
          InterleavedPacket pkt;
          pkt.channel = view.channel;
          pkt.payload.assign(view.bytes.begin(), view.bytes.end());
          interleaved_inbox_.push_back(std::move(pkt));
          read_buf_.consume(view.wire_bytes);
          continue;
        }
        if (view.type == proto::rtsp::FramedMessageType::RtspRequest) {
          out.raw_text = std::string(view.as_text());
          read_buf_.consume(view.wire_bytes);
          out.interleaved_frames_before_reply = interleaved_inbox_.size() - initial_interleaved;
          parse_response_fields(out);
          return out;
        }
      }

      const auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now())
                              .count();
      if (rem_ms <= 0) {
        break;
      }
      struct pollfd pfd{};
      pfd.fd       = fd_;
      pfd.events   = POLLIN;
      const int pr = ::poll(&pfd, 1, static_cast<int>(rem_ms));
      if (pr <= 0) {
        continue;
      }
      auto span = read_buf_.writable_span(16384);
      if (span.empty()) {
        return out;
      }
      const ssize_t n = ::recv(fd_, span.data(), span.size(), MSG_DONTWAIT);
      if (n > 0) {
        read_buf_.commit_write(static_cast<size_t>(n));
      } else if (n == 0) {
        return out;
      }
    }
    return out;
  }

  /// Collects at least `min_count` TCP interleaved `$` frames (or returns what arrived before
  /// timeout).
  std::vector<InterleavedPacket> recv_interleaved(size_t min_count, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms * timeout_scale_factor());

    while (interleaved_inbox_.size() < min_count && std::chrono::steady_clock::now() < deadline &&
        fd_ >= 0) {
      while (!read_buf_.empty()) {
        auto view = read_buf_.next_view();
        if (view.type == proto::rtsp::FramedMessageType::None) {
          break;
        }
        if (view.type == proto::rtsp::FramedMessageType::Error) {
          goto done;
        }
        if (view.type == proto::rtsp::FramedMessageType::Interleaved) {
          InterleavedPacket pkt;
          pkt.channel = view.channel;
          pkt.payload.assign(view.bytes.begin(), view.bytes.end());
          interleaved_inbox_.push_back(std::move(pkt));
          read_buf_.consume(view.wire_bytes);
        } else {
          read_buf_.consume(view.wire_bytes);
        }
      }
      if (interleaved_inbox_.size() >= min_count) {
        break;
      }
      const auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now())
                              .count();
      if (rem_ms <= 0) {
        break;
      }
      struct pollfd pfd{};
      pfd.fd       = fd_;
      pfd.events   = POLLIN;
      const int pr = ::poll(&pfd, 1, static_cast<int>(rem_ms));
      if (pr <= 0) {
        continue;
      }
      auto span = read_buf_.writable_span(32768);
      if (span.empty()) {
        break;
      }
      const ssize_t n = ::recv(fd_, span.data(), span.size(), MSG_DONTWAIT);
      if (n > 0) {
        read_buf_.commit_write(static_cast<size_t>(n));
      } else if (n == 0) {
        break;
      }
    }

done:
    std::vector<InterleavedPacket> result;
    while (!interleaved_inbox_.empty()) {
      result.push_back(std::move(interleaved_inbox_.front()));
      interleaved_inbox_.pop_front();
    }
    return result;
  }

  /// Collects at least `min_count` UDP RTP packets on `udp_rtp_fd_`.
  std::vector<std::vector<uint8_t>> recv_udp_rtp(size_t min_count, int timeout_ms = 2000) {
    std::vector<std::vector<uint8_t>> out;
    if (udp_rtp_fd_ < 0) {
      return out;
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms * timeout_scale_factor());
    uint8_t buf[65536];

    while (out.size() < min_count && std::chrono::steady_clock::now() < deadline) {
      while (true) {
        const ssize_t n = ::recv(udp_rtp_fd_, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
          out.emplace_back(buf, buf + n);
        } else {
          break;
        }
      }
      if (out.size() >= min_count) {
        break;
      }
      const auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now())
                              .count();
      if (rem_ms <= 0) {
        break;
      }
      struct pollfd pfd{};
      pfd.fd     = udp_rtp_fd_;
      pfd.events = POLLIN;
      (void)::poll(&pfd, 1, static_cast<int>(rem_ms));
    }
    return out;
  }

  /// Checks whether peer closed TCP connection (EOF).
  bool wait_for_eof(int timeout_ms = 3500) {
    if (fd_ < 0) return true;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms * timeout_scale_factor());
    uint8_t buf[4096];
    while (std::chrono::steady_clock::now() < deadline) {
      const ssize_t n = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
      if (n == 0) {
        return true;
      }
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return true;
      }
      struct pollfd pfd{};
      pfd.fd     = fd_;
      pfd.events = POLLIN;
      (void)::poll(&pfd, 1, 20);
    }
    return false;
  }

  /// Extracts `Session: <id>` from any raw RTSP response text.
  [[nodiscard]] static std::string extract_session_header(std::string_view resp) {
    const auto pos = resp.find("Session:");
    if (pos == std::string_view::npos) {
      return {};
    }
    auto val = resp.substr(pos + 8);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
      val.remove_prefix(1);
    }
    const auto end = val.find_first_of(";\r\n ");
    return std::string(val.substr(0, end));
  }

  /// Extracts Digest Auth `nonce="..."` from a `401 Unauthorized` response.
  [[nodiscard]] static std::string extract_digest_nonce(std::string_view resp) {
    const auto pos = resp.find("nonce=\"");
    if (pos == std::string_view::npos) {
      return {};
    }
    auto       val = resp.substr(pos + 7);
    const auto q   = val.find('"');
    if (q == std::string_view::npos) {
      return {};
    }
    return std::string(val.substr(0, q));
  }

  /// Builds an RFC 2617 `Authorization: Digest ...` header line (`\r\n` terminated).
  [[nodiscard]] static std::string make_digest_auth_header(std::string_view method,
      std::string_view                                                      uri,
      std::string_view                                                      realm,
      std::string_view                                                      nonce,
      std::string_view                                                      user,
      std::string_view                                                      pass) {
    const std::string ha1 = proto::rtsp::md5_detail::md5_hex(
        std::string(user) + ":" + std::string(realm) + ":" + std::string(pass));
    const std::string ha2 =
        proto::rtsp::md5_detail::md5_hex(std::string(method) + ":" + std::string(uri));
    const std::string response =
        proto::rtsp::md5_detail::md5_hex(ha1 + ":" + std::string(nonce) + ":" + ha2);

    return "Authorization: Digest username=\"" + std::string(user) + "\", realm=\"" +
        std::string(realm) + "\", nonce=\"" + std::string(nonce) + "\", uri=\"" + std::string(uri) +
        "\", response=\"" + response + "\"\r\n";
  }

private:
  void parse_response_fields(RtspClientResponse &r) {
    const auto sep = r.raw_text.find("\r\n\r\n");
    if (sep != std::string::npos) {
      r.headers = r.raw_text.substr(0, sep);
      r.body    = r.raw_text.substr(sep + 4);
    } else {
      r.headers = r.raw_text;
    }

    /// Status line: `RTSP/1.0 <code> <reason>`
    const auto sp1 = r.headers.find(' ');
    if (sp1 != std::string::npos) {
      const auto sp2 = r.headers.find(' ', sp1 + 1);
      if (sp2 != std::string::npos) {
        if (auto c = config::parse_uint<uint16_t>(
                std::string_view(r.headers).substr(sp1 + 1, sp2 - sp1 - 1))) {
          r.status_code = static_cast<int>(*c);
        }
      }
    }

    if (std::string sid = extract_session_header(r.headers); !sid.empty()) {
      session_id_ = std::move(sid);
    }

    const auto sp_pos = r.headers.find("server_port=");
    if (sp_pos != std::string::npos) {
      std::string_view sv(r.headers);
      sv.remove_prefix(sp_pos + 12);
      const auto end  = sv.find_first_of(";\r\n");
      sv              = sv.substr(0, end);
      const auto dash = sv.find('-');
      if (dash != std::string_view::npos) {
        if (auto p0 = config::parse_uint<uint16_t>(sv.substr(0, dash))) {
          server_rtp_port_ = *p0;
        }
        if (auto p1 = config::parse_uint<uint16_t>(sv.substr(dash + 1))) {
          server_rtcp_port_ = *p1;
        }
      }
    }
  }
};

} //< namespace rivet::test
