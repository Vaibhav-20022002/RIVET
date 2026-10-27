#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace rivet::proto::rtp {

/// Decoded RTP packet metadata and media classification flags
struct RtpPacketInfo {
  uint16_t seq{0};
  uint32_t timestamp{0};
  uint32_t ssrc{0};
  uint32_t header_len{12};
  uint32_t payload_len{0};
  bool     marker{false};
  uint8_t  track_id{0};
  /// Bit 0: IDR, Bit 1: CRA, Bit 2: Ref Pic, Bit 3: Marker, Bit 4: RASL
  uint8_t flags{0};
  bool    valid{false};
};

/// High-performance, zero-copy RTP packet and H.264/AAC classifier
class RtpPacketClassifier {
public:
  /// Classifies a raw RTP packet buffer according to RFC 3550 and RFC 6184
  ///\param data Pointer to raw packet data
  ///\param len Total length of the packet in bytes
  ///\param track_id Track identifier (0 = Video, 1 = Audio)
  ///\return RtpPacketInfo containing parsed fields and priority flags
  [[nodiscard]] static RtpPacketInfo
  classify(const uint8_t *data, size_t len, uint8_t track_id) noexcept {
    RtpPacketInfo info;
    info.track_id = track_id;

    if (!data || len < 12) {
      return info;
    }

    /// RFC 3550 Section 5.1
    uint8_t v_p_x_cc = data[0];
    uint8_t version  = (v_p_x_cc >> 6) & 0x03;
    if (version != 2) {
      return info; //< Unsupported RTP version
    }

    bool    has_padding   = (v_p_x_cc & 0x20) != 0;
    bool    has_extension = (v_p_x_cc & 0x10) != 0;
    uint8_t cc            = v_p_x_cc & 0x0F;

    info.marker    = (data[1] & 0x80) != 0;
    info.seq       = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    info.timestamp = (static_cast<uint32_t>(data[4]) << 24) |
        (static_cast<uint32_t>(data[5]) << 16) | (static_cast<uint32_t>(data[6]) << 8) | data[7];
    info.ssrc = (static_cast<uint32_t>(data[8]) << 24) | (static_cast<uint32_t>(data[9]) << 16) |
        (static_cast<uint32_t>(data[10]) << 8) | data[11];

    size_t header_len = 12 + (static_cast<size_t>(cc) * 4);
    if (len < header_len) {
      return info;
    }

    if (has_extension) {
      if (len < header_len + 4) {
        return info;
      }
      uint16_t ext_words =
          (static_cast<uint16_t>(data[header_len + 2]) << 8) | data[header_len + 3];
      header_len += 4 + (static_cast<size_t>(ext_words) * 4);
      if (len < header_len) {
        return info;
      }
    }

    size_t padding_len = 0;
    if (has_padding) {
      padding_len = data[len - 1];
      if (header_len + padding_len > len) {
        return info;
      }
    }

    info.header_len  = static_cast<uint32_t>(header_len);
    info.payload_len = static_cast<uint32_t>(len - header_len - padding_len);

    if (info.marker) {
      info.flags |= 0x08;
    }

    /// Audio Track (track_id == 1)
    if (track_id == 1) {
      info.flags |= 0x04; //< Audio packets are essential reference data
      info.valid = true;
      return info;
    }

    /// Video Track (track_id == 0) - H.264 RFC 6184 classification
    const uint8_t *payload = data + header_len;
    size_t         plen    = info.payload_len;

    if (plen > 0) {
      uint8_t nal_type    = payload[0] & 0x1F;
      uint8_t nal_ref_idc = (payload[0] >> 5) & 0x03;

      if (nal_type == 28 && plen > 1) {
        /// FU-A fragmented NAL unit
        uint8_t fu_type  = payload[1] & 0x1F;
        bool    fu_start = (payload[1] & 0x80) != 0;

        if (nal_ref_idc != 0) {
          info.flags |= 0x04; //< Reference picture slice
        }
        if (fu_type == 5) {
          info.flags |= 0x04; //< All IDR fragments are reference
          if (fu_start) {
            info.flags |= 0x01; //< IDR Keyframe start fragment
          }
        }
      } else if (nal_type == 24 && plen > 1) {
        /// STAP-A aggregation packet: scan NAL units inside
        size_t off = 1;
        while (off + 2 <= plen) {
          uint16_t nalu_size = (static_cast<uint16_t>(payload[off]) << 8) | payload[off + 1];
          off += 2;
          if (off + nalu_size > plen || nalu_size == 0) {
            break;
          }
          uint8_t sub_type = payload[off] & 0x1F;
          uint8_t sub_nri  = (payload[off] >> 5) & 0x03;
          if (sub_nri != 0) {
            info.flags |= 0x04;
          }
          if (sub_type == 5) {
            info.flags |= 0x05;
          }
          off += nalu_size;
        }
      } else {
        /// Single NAL unit packet
        if (nal_ref_idc != 0) {
          info.flags |= 0x04; //< Reference picture slice, SPS, PPS
        }
        if (nal_type == 5) {
          info.flags |= 0x05; //< IDR Keyframe (bit 0 + bit 2)
        }
      }
    }

    info.valid = true;
    return info;
  }
};

} //< namespace rivet::proto::rtp
