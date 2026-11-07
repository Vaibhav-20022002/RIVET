#pragma once

#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/Zones.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::proto::rtsp {

/// Type of framed message extracted from the RTSP TCP stream
enum class FramedMessageType : uint8_t {
  None,        //< Incomplete, need more data
  Interleaved, //< Interleaved binary RTP/RTCP packet ($ + channel + len + payload)
  RtspRequest, //< Complete text RTSP request
  Error        //< Protocol framing violation or oversized message
};

/// Owning result of parsing an RTSP stream frame (retained for `next_frame(std::string&)` callers)
struct FramedMessage {
  FramedMessageType type{FramedMessageType::None};
  uint8_t           channel{0};
  uint32_t          length{0}; //< Widened from uint16_t so >64KB RTSP bodies never wrap!
  std::string       data;
};

/// Zero-copy view of a framed message inside an `RtspReadBuffer`.
/// Valid until `RtspReadBuffer::consume(wire_bytes)` is invoked.
struct FramedView {
  FramedMessageType type{FramedMessageType::None};
  uint8_t           channel{0};
  uint32_t          length{0};     //< Payload length for Interleaved; total bytes for RtspRequest
  uint32_t          wire_bytes{0}; //< Total wire bytes to advance `read_pos_` (> 0 on match)
  std::span<const uint8_t> bytes{};

  [[nodiscard]] std::string_view as_text() const noexcept {
    return std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
};

/// Hardened RFC 2326 / RFC 7826 stream framer with zero-allocation view parsing,
/// strict `std::from_chars` unsigned `Content-Length` validation, O(n) incremental header
/// scanning, and full 65,535-byte interleaved payload support.
class RtspStreamFramer {
  [[nodiscard]] static bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      char ca = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 32) : a[i];
      char cb = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 32) : b[i];
      if (ca != cb) return false;
    }
    return true;
  }

  [[nodiscard]] static std::string_view trim_ows(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
    return sv;
  }

public:
  /// Maximum allowed RTSP control header block before disconnect (Slowloris defense)
  static constexpr size_t MAX_HEADER_SIZE = 16 * 1024; //< 16 KiB

  /// Maximum allowed RTSP request body (16 KiB SDP limit; interleaved `$` frames use 65535)
  static constexpr size_t MAX_BODY_SIZE = 16 * 1024; //< 16 KiB

  /// Maximum RFC 2326 §10.12 16-bit interleaved `$` payload length
  static constexpr size_t MAX_INTERLEAVED_FRAME_SIZE = 65535;

  /// Core zero-allocation parser operating over an arbitrary contiguous byte window.
  /// `scan_hint` is the byte offset within `buf` from which `\r\n\r\n` has not yet been searched,
  /// guaranteeing O(n) total work across fragmented reads!
  [[nodiscard]] static FramedView parse_view(std::span<const uint8_t> buf,
      size_t                                                         &scan_hint,
      size_t max_interleaved_size = MAX_INTERLEAVED_FRAME_SIZE) noexcept {
    RIVET_ZONE("Rtsp::parse_view");
    FramedView view{};
    if (buf.empty()) {
      scan_hint = 0;
      return view;
    }

    /// 1. Interleaved binary frame: '$' <channel:8> <length_be:16> <payload>
    if (buf[0] == static_cast<uint8_t>('$')) {
      scan_hint = 0;
      if (buf.size() < 4U) {
        return view;
      }

      const uint8_t  channel = buf[1];
      const uint16_t plen = static_cast<uint16_t>((static_cast<uint16_t>(buf[2]) << 8U) | buf[3]);

      if (static_cast<size_t>(plen) > max_interleaved_size) {
        view.type = FramedMessageType::Error;
        return view;
      }

      const size_t total_wire = 4U + static_cast<size_t>(plen);
      if (buf.size() < total_wire) {
        return view;
      }

      view.type       = FramedMessageType::Interleaved;
      view.channel    = channel;
      view.length     = plen;
      view.wire_bytes = static_cast<uint32_t>(total_wire);
      view.bytes      = buf.subspan(4U, plen);
      return view;
    }

    /// Reject binary garbage / control bytes at start of an RTSP text request line
    if (buf[0] < 0x21U || buf[0] > 0x7EU) {
      view.type = FramedMessageType::Error;
      return view;
    }

    /// 2. RTSP text request: incremental O(n) scan for "\r\n\r\n"
    const std::string_view text(reinterpret_cast<const char *>(buf.data()), buf.size());
    const size_t           search_from = (scan_hint >= 3U) ? (scan_hint - 3U) : 0U;
    const size_t           hdr_end     = text.find("\r\n\r\n", search_from);

    if (hdr_end == std::string_view::npos) {
      if (buf.size() > MAX_HEADER_SIZE) {
        view.type = FramedMessageType::Error;
        return view;
      }
      scan_hint = buf.size();
      return view;
    }

    if (hdr_end > MAX_HEADER_SIZE) {
      view.type = FramedMessageType::Error;
      return view;
    }

    /// Validate header lines and extract line-anchored `Content-Length` using `std::from_chars`
    const std::string_view header_block = text.substr(0, hdr_end);
    const size_t           req_line_end = header_block.find("\r\n");
    uint32_t               content_len  = 0U;
    bool                   saw_clen     = false;

    if (req_line_end != std::string_view::npos) {
      size_t pos = req_line_end + 2U;
      while (pos < header_block.size()) {
        const size_t next_crlf = header_block.find("\r\n", pos);
        const auto   hline     = (next_crlf == std::string_view::npos)
            ? header_block.substr(pos)
            : header_block.substr(pos, next_crlf - pos);
        pos = (next_crlf == std::string_view::npos) ? header_block.size() : (next_crlf + 2U);
        if (hline.empty()) continue;

        const size_t colon = hline.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
          view.type = FramedMessageType::Error;
          return view;
        }

        const auto hname = trim_ows(hline.substr(0, colon));
        const auto hval  = trim_ows(hline.substr(colon + 1U));

        if (iequals_ascii(hname, "Content-Length")) {
          auto parsed =
              config::parse_uint<uint32_t>(hval, 0U, static_cast<uint32_t>(MAX_BODY_SIZE));
          if (!parsed) {
            /// Rejects negative numbers (`-43` livelock!), `+`, garbage, or > 64 KiB bodies
            view.type = FramedMessageType::Error;
            return view;
          }
          if (saw_clen && *parsed != content_len) {
            /// Conflicting duplicate Content-Length headers -> request smuggling attempt!
            view.type = FramedMessageType::Error;
            return view;
          }
          content_len = *parsed;
          saw_clen    = true;
        }
      }
    }

    const size_t total_req_bytes = hdr_end + 4U + static_cast<size_t>(content_len);
    if (buf.size() < total_req_bytes) {
      /// Headers are complete; pin `scan_hint` at `hdr_end` so next recv jumps straight to `\r\n\r\n`
      scan_hint = hdr_end;
      return view;
    }

    assert(total_req_bytes >= 4U);
    scan_hint       = 0;
    view.type       = FramedMessageType::RtspRequest;
    view.length     = static_cast<uint32_t>(total_req_bytes);
    view.wire_bytes = static_cast<uint32_t>(total_req_bytes);
    view.bytes      = buf.subspan(0U, total_req_bytes);
    return view;
  }

  /// Compatibility wrapper operating on `std::string &recv_buf` (used by unit tests & benchmarks).
  static FramedMessage next_frame(std::string &recv_buf,
      size_t                                   max_slab_size = MAX_INTERLEAVED_FRAME_SIZE) {
    FramedMessage msg{};
    if (recv_buf.empty()) {
      return msg;
    }

    size_t     scan_hint = 0;
    const auto span      = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t *>(recv_buf.data()), recv_buf.size());
    const FramedView view = parse_view(span, scan_hint, max_slab_size);

    msg.type    = view.type;
    msg.channel = view.channel;
    msg.length  = view.length;

    if (view.type == FramedMessageType::Error) {
      recv_buf.clear();
      return msg;
    }
    if (view.type == FramedMessageType::None) {
      return msg;
    }

    assert(view.wire_bytes > 0U && view.wire_bytes <= recv_buf.size());
    msg.data.assign(reinterpret_cast<const char *>(view.bytes.data()), view.bytes.size());
    recv_buf.erase(0U, view.wire_bytes);
    return msg;
  }
};

/// Pre-allocated, zero-churn linear compaction buffer for per-connection RTSP / `$` framing.
/// Eliminates per-frame `std::string` allocations and `erase(0, n)` shifts on TCP sockets.
class RtspReadBuffer {
public:
  static constexpr size_t DEFAULT_CAPACITY = 128 * 1024; //< Fits 65,535 B `$` frame or 64 KB body

private:
  std::vector<uint8_t> storage_;
  size_t               read_pos_{0};
  size_t               write_pos_{0};
  size_t               scan_hint_{0};

public:
  explicit RtspReadBuffer(size_t cap = DEFAULT_CAPACITY)
      : storage_(cap) {}

  [[nodiscard]] size_t capacity() const noexcept { return storage_.size(); }
  [[nodiscard]] size_t readable_bytes() const noexcept { return write_pos_ - read_pos_; }
  [[nodiscard]] bool   empty() const noexcept { return read_pos_ == write_pos_; }

  /// Compacts unread tail to offset 0 when write space runs low (amortized O(1)).
  void ensure_writable(size_t min_space = 8192) noexcept {
    if (read_pos_ == write_pos_) {
      read_pos_  = 0;
      write_pos_ = 0;
      scan_hint_ = 0;
      return;
    }
    if (storage_.size() - write_pos_ < min_space && read_pos_ > 0U) {
      const size_t rem = write_pos_ - read_pos_;
      std::memmove(storage_.data(), storage_.data() + read_pos_, rem);
      read_pos_  = 0;
      write_pos_ = rem;
    }
  }

  [[nodiscard]] std::span<uint8_t> writable_span(size_t min_space = 8192) noexcept {
    ensure_writable(min_space);
    return std::span<uint8_t>{storage_.data() + write_pos_, storage_.size() - write_pos_};
  }

  void commit_write(size_t n) noexcept {
    assert(write_pos_ + n <= storage_.size());
    write_pos_ += n;
  }

  bool append(const void *src, size_t len) noexcept {
    if (len == 0U) return true;
    ensure_writable(len);
    if (storage_.size() - write_pos_ < len) {
      return false;
    }
    std::memcpy(storage_.data() + write_pos_, src, len);
    write_pos_ += len;
    return true;
  }

  [[nodiscard]] std::span<const uint8_t> readable_span() const noexcept {
    return std::span<const uint8_t>{storage_.data() + read_pos_, write_pos_ - read_pos_};
  }

  [[nodiscard]] FramedView next_view(
      size_t max_interleaved = RtspStreamFramer::MAX_INTERLEAVED_FRAME_SIZE) noexcept {
    return RtspStreamFramer::parse_view(readable_span(), scan_hint_, max_interleaved);
  }

  void consume(size_t wire_bytes) noexcept {
    assert(wire_bytes <= readable_bytes());
    read_pos_ += wire_bytes;
    scan_hint_ = 0;
    if (read_pos_ == write_pos_) {
      read_pos_  = 0;
      write_pos_ = 0;
    }
  }

  void clear() noexcept {
    read_pos_  = 0;
    write_pos_ = 0;
    scan_hint_ = 0;
  }
};

} //< namespace rivet::proto::rtsp
