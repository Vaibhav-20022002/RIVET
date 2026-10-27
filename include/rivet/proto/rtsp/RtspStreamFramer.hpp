#pragma once

#include "rivet/core/Zones.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace rivet::proto::rtsp {

/// Type of framed message extracted from the RTSP TCP stream
enum class FramedMessageType : uint8_t {
  None,        //< Incomplete, need more data
  Interleaved, //< Interleaved binary RTP/RTCP packet ($ + channel + len + payload)
  RtspRequest, //< Complete text RTSP request
  Error        //< Protocol framing violation or oversized message
};

/// Result of parsing an RTSP stream frame
struct FramedMessage {
  FramedMessageType type{FramedMessageType::None};
  uint8_t           channel{0};
  uint16_t          length{0};
  std::string       data;
};

/// Robust stream framer handling RFC 2326 Section 10.12 embedded binary framing
/// and RFC 2326 text command parsing with defensive size bounds.
class RtspStreamFramer {
public:
  /// Maximum allowed RTSP control header size before disconnect (prevents Slowloris DoS)
  static constexpr size_t MAX_HEADER_SIZE = 64 * 1024; //< 64 KB

  /// Maximum allowed interleaved payload size (prevents slab memory overruns)
  static constexpr size_t MAX_INTERLEAVED_FRAME_SIZE = 2048;

  /// Attempts to extract the next complete message from the receive buffer
  ///\param recv_buf In-out receive buffer. Consumed bytes are erased from the front.
  ///\param max_slab_size Upper bound on interleaved packet payload
  ///\return FramedMessage containing the extracted message or indicating incomplete/error
  static FramedMessage next_frame(std::string &recv_buf,
      size_t                                   max_slab_size = MAX_INTERLEAVED_FRAME_SIZE) {
    RIVET_ZONE("Rtsp::next_frame");
    FramedMessage msg;
    if (recv_buf.empty()) {
      return msg;
    }

    /// 1. Interleaved binary frame: '$' <channel> <length_hi> <length_lo>
    if (recv_buf[0] == '$') {
      if (recv_buf.size() < 4) {
        return msg; //< Need header
      }

      uint8_t  channel = static_cast<uint8_t>(recv_buf[1]);
      uint16_t plen = (static_cast<uint8_t>(recv_buf[2]) << 8) | static_cast<uint8_t>(recv_buf[3]);

      /// Defense against heap corruption (Defect D1)
      if (plen > max_slab_size) {
        msg.type = FramedMessageType::Error;
        recv_buf.clear(); //< Drop corrupt stream
        return msg;
      }

      if (recv_buf.size() < static_cast<size_t>(4 + plen)) {
        return msg; //< Need rest of payload
      }

      msg.type    = FramedMessageType::Interleaved;
      msg.channel = channel;
      msg.length  = plen;
      msg.data.assign(recv_buf.data() + 4, plen);
      recv_buf.erase(0, 4 + plen);
      return msg;
    }

    /// 2. RTSP text request
    auto hdr_end = recv_buf.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
      /// Defense against unbounded memory growth (Defect D11)
      if (recv_buf.size() > MAX_HEADER_SIZE) {
        msg.type = FramedMessageType::Error;
        recv_buf.clear();
        return msg;
      }
      return msg; //< Incomplete headers
    }

    size_t req_len = hdr_end + 4;

    /// Check for optional Content-Length body
    auto cl_pos = recv_buf.find("Content-Length: ");
    if (cl_pos == std::string::npos) {
      cl_pos = recv_buf.find("content-length: ");
    }

    if (cl_pos != std::string::npos && cl_pos < hdr_end) {
      size_t clen = std::strtoul(recv_buf.c_str() + cl_pos + 16, nullptr, 10);
      if (recv_buf.size() < hdr_end + 4 + clen) {
        return msg; //< Incomplete body
      }
      req_len = hdr_end + 4 + clen;
    }

    msg.type   = FramedMessageType::RtspRequest;
    msg.length = static_cast<uint16_t>(req_len);
    msg.data   = recv_buf.substr(0, req_len);
    recv_buf.erase(0, req_len);
    return msg;
  }
};

} //< namespace rivet::proto::rtsp
