#pragma once

#include "rivet/media/HevcPayload.hpp"
#include "rivet/media/StreamDescriptor.hpp"
#include "rivet/memory/PacketFlags.hpp"
#include <cstddef>
#include <cstdint>

namespace rivet::proto::rtp {

/// Decoded RTP packet metadata and codec classification flags
struct RtpPacketInfo {
  uint16_t seq{0};
  uint32_t timestamp{0};
  uint32_t ssrc{0};
  uint32_t header_len{12};
  uint32_t payload_len{0};
  uint8_t  payload_type{0};
  bool     marker{false};
  uint8_t  track_id{0};          //< Plain track index (0..7)
  uint8_t  packed_track_meta{0}; //< Packed Byte 30 for PacketRef (track + tid + vcl + switch_up)
  uint8_t  flags{0};             //< Byte 31 flags (`memory::pkt::k*`)
  bool     valid{false};
  bool     is_rtcp{false}; //< True when packet is RTCP (PT 200..207 / 72..76)

  /// Fine-grained AU & RFC 7798 diagnostics for IngestReactor & Metrics
  bool                         is_fu{false};
  bool                         fu_start{false};
  bool                         fu_end{false};
  bool                         first_slice{false};
  bool                         hevc_malformed{false};
  bool                         hevc_nonbase_layer{false};
  media::hevc::MalformedReason hevc_reason{media::hevc::MalformedReason::None};
};

/// High-performance, zero-allocation RTP header validator and H.264 / HEVC / Audio classifier.
class RtpPacketClassifier {
  /// Parses and validates the RFC 3550 fixed header, CSRCs, header extension, and padding,
  /// rejecting RTCP packets in the RFC 5761 multiplex range (PT 200..207 / 72..76).
  [[nodiscard]] static bool
  parse_rtp_envelope(const uint8_t *data, size_t len, RtpPacketInfo &info) noexcept {
    if (!data || len < 12U) {
      return false;
    }

    const uint8_t v_p_x_cc = data[0];
    const uint8_t version  = (v_p_x_cc >> 6U) & 0x03U;
    if (version != 2U) {
      return false;
    }

    /// RFC 5761 §4 RTCP vs RTP demultiplexing check.
    /// RTCP SR(200), RR(201), SDES(202), BYE(203), APP(204), RTPFB(205), PSFB(206), XR(207)
    /// and reserved conflict-avoidance PTs 72..76 MUST NEVER enter the RTP media pipeline.
    const uint8_t raw_b1 = data[1];
    const uint8_t pt7    = raw_b1 & 0x7FU;
    if ((raw_b1 >= 200U && raw_b1 <= 207U) || (pt7 >= 72U && pt7 <= 76U)) {
      info.is_rtcp = true;
      return false;
    }

    const bool    has_padding   = (v_p_x_cc & 0x20U) != 0;
    const bool    has_extension = (v_p_x_cc & 0x10U) != 0;
    const uint8_t cc            = v_p_x_cc & 0x0FU;

    info.marker       = (raw_b1 & 0x80U) != 0;
    info.payload_type = pt7;
    info.seq          = static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8U) | data[3]);
    info.timestamp    = (static_cast<uint32_t>(data[4]) << 24U) |
        (static_cast<uint32_t>(data[5]) << 16U) | (static_cast<uint32_t>(data[6]) << 8U) | data[7];
    info.ssrc = (static_cast<uint32_t>(data[8]) << 24U) | (static_cast<uint32_t>(data[9]) << 16U) |
        (static_cast<uint32_t>(data[10]) << 8U) | data[11];

    size_t header_len = 12U + (static_cast<size_t>(cc) * 4U);
    if (len < header_len) {
      return false;
    }

    if (has_extension) {
      if (len < header_len + 4U) {
        return false;
      }
      const uint16_t ext_words = static_cast<uint16_t>(
          (static_cast<uint16_t>(data[header_len + 2U]) << 8U) | data[header_len + 3U]);
      header_len += 4U + (static_cast<size_t>(ext_words) * 4U);
      if (len < header_len) {
        return false;
      }
    }

    size_t padding_len = 0U;
    if (has_padding) {
      padding_len = data[len - 1U];
      if (padding_len == 0U || header_len + padding_len > len) {
        return false;
      }
    }

    info.header_len  = static_cast<uint32_t>(header_len);
    info.payload_len = static_cast<uint32_t>(len - header_len - padding_len);
    if (info.marker) {
      info.flags |= memory::pkt::kMarker;
    }
    return true;
  }

  static void
  classify_h264_payload(const uint8_t *payload, size_t plen, RtpPacketInfo &info) noexcept {
    using namespace memory::pkt;
    bool is_vcl = false;

    if (plen > 0U && (payload[0] & 0x80U) == 0U) {
      const uint8_t nal_type    = payload[0] & 0x1FU;
      const uint8_t nal_ref_idc = (payload[0] >> 5U) & 0x03U;

      if (nal_type == 28U && plen > 1U) {
        /// RFC 6184 §5.8 FU-A fragmented NAL unit
        const uint8_t fu_type  = payload[1] & 0x1FU;
        const bool    fu_start = (payload[1] & 0x80U) != 0;
        const bool    fu_end   = (payload[1] & 0x40U) != 0;

        info.is_fu    = true;
        info.fu_start = fu_start;
        info.fu_end   = fu_end;

        if (fu_type >= 1U && fu_type <= 5U) {
          is_vcl = true;
        }
        if (nal_ref_idc != 0U) {
          info.flags |= kRef;
        }
        if (fu_type == 5U) {
          info.flags |= kRef;
          if (fu_start) {
            info.flags |= kClosedIrap;
          }
        } else if (fu_type == 7U || fu_type == 8U) {
          info.flags |= kParamSet;
        }
      } else if (nal_type == 24U && plen > 1U) {
        /// RFC 6184 §5.7 STAP-A aggregation packet
        size_t off = 1U;
        while (off + 2U <= plen) {
          const uint16_t nalu_size = static_cast<uint16_t>(
              (static_cast<uint16_t>(payload[off]) << 8U) | payload[off + 1U]);
          off += 2U;
          if (nalu_size == 0U || off + nalu_size > plen) {
            break;
          }
          if ((payload[off] & 0x80U) == 0U) {
            const uint8_t sub_type = payload[off] & 0x1FU;
            const uint8_t sub_nri  = (payload[off] >> 5U) & 0x03U;
            if (sub_type >= 1U && sub_type <= 5U) {
              is_vcl = true;
            }
            if (sub_nri != 0U) {
              info.flags |= kRef;
            }
            if (sub_type == 5U) {
              info.flags |= static_cast<uint8_t>(kClosedIrap | kRef);
            } else if (sub_type == 7U || sub_type == 8U) {
              info.flags |= kParamSet;
            }
          }
          off += nalu_size;
        }
      } else {
        /// Single NAL unit packet
        if (nal_type >= 1U && nal_type <= 5U) {
          is_vcl = true;
        }
        if (nal_ref_idc != 0U) {
          info.flags |= kRef;
        }
        if (nal_type == 5U) {
          info.flags |= static_cast<uint8_t>(kClosedIrap | kRef);
        } else if (nal_type == 7U || nal_type == 8U) {
          info.flags |= kParamSet;
        }
      }
    }

    /// H.264 uses temporal_id = 0, switch_up = false
    info.packed_track_meta = memory::pkt::pack_track_meta(info.track_id, 0U, is_vcl, false);
  }

public:
  /// Codec-aware classifier driven by `TrackDescriptor` (H.264, HEVC RFC 7798, Audio, Opaque).
  [[nodiscard]] static RtpPacketInfo
  classify(const uint8_t *data, size_t len, const media::TrackDescriptor &track) noexcept {
    RtpPacketInfo info{};
    info.track_id          = track.index;
    info.packed_track_meta = memory::pkt::pack_track_meta(track.index, 0U, false, false);

    if (!parse_rtp_envelope(data, len, info)) {
      return info;
    }

    const uint8_t *payload = data + info.header_len;
    const size_t   plen    = info.payload_len;

    if (track.kind != media::MediaKind::Video) {
      /// Audio / non-video tracks are always treated as essential reference packets
      info.flags |= memory::pkt::kRef;
      info.packed_track_meta = memory::pkt::pack_track_meta(track.index, 7U, false, false);
      info.valid             = true;
      return info;
    }

    if (track.codec == media::VideoCodec::H265) {
      const bool has_don = (track.hevc.sprop_max_don_diff > 0U);
      const auto hc      = media::hevc::classify(payload, plen, has_don);
      info.flags |= hc.flags;
      info.is_fu              = hc.is_fu;
      info.fu_start           = hc.fu_start;
      info.fu_end             = hc.fu_end;
      info.first_slice        = hc.first_slice;
      info.hevc_malformed     = hc.malformed;
      info.hevc_nonbase_layer = hc.nonbase_layer;
      info.hevc_reason        = hc.reason;
      const uint8_t tid       = hc.vcl ? hc.tid : 7U;
      info.packed_track_meta = memory::pkt::pack_track_meta(track.index, tid, hc.vcl, hc.switch_up);
      info.valid             = true;
      return info;
    }

    if (track.codec == media::VideoCodec::H264) {
      classify_h264_payload(payload, plen, info);
      info.valid = true;
      return info;
    }

    /// Unknown video codec: forward opaquely with `kRef` set
    info.flags |= memory::pkt::kRef;
    info.packed_track_meta = memory::pkt::pack_track_meta(track.index, 7U, false, false);
    info.valid             = true;
    return info;
  }

  /// Legacy 3-argument overload (`track_id == 0` -> H.264 Video, `track_id >= 1` -> Audio).
  [[nodiscard]] static RtpPacketInfo
  classify(const uint8_t *data, size_t len, uint8_t track_id) noexcept {
    media::TrackDescriptor trk{};
    trk.index = static_cast<uint8_t>(track_id & 0x07U);
    if (track_id == 0U) {
      trk.kind  = media::MediaKind::Video;
      trk.codec = media::VideoCodec::H264;
    } else {
      trk.kind  = media::MediaKind::Audio;
      trk.codec = media::VideoCodec::Unknown;
    }
    return classify(data, len, trk);
  }
};

} //< namespace rivet::proto::rtp
