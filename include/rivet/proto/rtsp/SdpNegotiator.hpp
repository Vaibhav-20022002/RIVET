#pragma once

#include "rivet/media/Base64.hpp"
#include "rivet/media/ConfigGeneration.hpp"
#include "rivet/media/StreamDescriptor.hpp"
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::proto::rtsp {

/// RFC 8866, RFC 6184 (H.264), and RFC 7798 (HEVC/H.265) declarative SDP parser & renderer.
class SdpNegotiator {
  static std::string join_base64_nals(const std::vector<std::vector<uint8_t>> &nals,
      const std::vector<uint8_t>                                              &fallback_single) {
    std::string out;
    if (!nals.empty()) {
      for (size_t i = 0; i < nals.size(); ++i) {
        if (i > 0U) out.push_back(',');
        out += media::base64::encode(nals[i]);
      }
    } else if (!fallback_single.empty()) {
      out += media::base64::encode(fallback_single);
    }
    return out;
  }

public:
  SdpNegotiator() = default;

  /// Validates an incoming SDP string using the full RFC 8866 `parse_announce_sdp` parser.
  [[nodiscard]] bool parse_camera_sdp(std::string_view sdp) const {
    auto res = media::parse_announce_sdp(sdp);
    return res.error == media::SdpError::None && res.descriptor.has_value();
  }

  /// Renders a clean, declarative RFC 8866 / RFC 7798 / RFC 6184 SDP from the validated
  /// `StreamDescriptor` and active `ConfigGeneration` snapshot (never echoing raw publisher text).
  [[nodiscard]] static std::string render_sdp(const media::StreamDescriptor &desc,
      const media::ConfigGeneration                                         &cfg,
      std::string_view origin_ip = "0.0.0.0") {
    const uint64_t sdp_ver = (static_cast<uint64_t>(desc.version) << 16U) +
        static_cast<uint64_t>(cfg.generation_id + 1U);

    std::string sdp;
    sdp.reserve(512);
    sdp += "v=0\r\n";
    sdp += "o=- 1700000000 " + std::to_string(sdp_ver) + " IN IP4 ";
    sdp.append(origin_ip.empty() ? std::string_view("0.0.0.0") : origin_ip);
    sdp += "\r\n";
    sdp += "s=RIVET\r\n";
    sdp += "c=IN IP4 0.0.0.0\r\n";
    sdp += "t=0 0\r\n";
    sdp += "a=control:*\r\n";

    for (const auto &trk : desc.tracks) {
      const char       *media_str = (trk.kind == media::MediaKind::Video) ? "video"
          : (trk.kind == media::MediaKind::Audio)                         ? "audio"
                                                                          : "application";
      const std::string pt_str    = std::to_string(trk.payload_type);

      sdp += "m=";
      sdp += media_str;
      sdp += " 0 RTP/AVP " + pt_str + "\r\n";

      if (!trk.encoding_name.empty()) {
        sdp +=
            "a=rtpmap:" + pt_str + " " + trk.encoding_name + "/" + std::to_string(trk.clock_rate);
        if (trk.kind == media::MediaKind::Audio && trk.channels > 1U) {
          sdp += "/" + std::to_string(trk.channels);
        }
        sdp += "\r\n";
      }

      if (trk.kind == media::MediaKind::Video && trk.codec == media::VideoCodec::H265) {
        /// RFC 7798 §7.2.3 Declarative HEVC fmtp
        uint8_t pspace   = trk.hevc.profile_space;
        uint8_t pid      = trk.hevc.profile_id;
        uint8_t tflag    = trk.hevc.tier_flag;
        uint8_t lid      = trk.hevc.level_id;
        bool    have_ptl = trk.hevc.has_explicit_ptl;

        std::string compat_hex = trk.hevc.profile_compatibility_indicator;
        std::string constr_hex = trk.hevc.interop_constraints;

        if (cfg.hevc_ptl.has_value()) {
          pspace   = cfg.hevc_ptl->profile_space;
          pid      = cfg.hevc_ptl->profile_idc;
          tflag    = cfg.hevc_ptl->tier_flag;
          lid      = cfg.hevc_ptl->level_idc;
          have_ptl = true;

          char cbuf[9]{};
          std::snprintf(cbuf, sizeof(cbuf), "%08x", cfg.hevc_ptl->compat_flags);
          compat_hex.assign(cbuf, 8);

          char ibuf[13]{};
          std::snprintf(ibuf,
              sizeof(ibuf),
              "%012llx",
              static_cast<unsigned long long>(
                  cfg.hevc_ptl->constraint_flags & 0x0000FFFFFFFFFFFFULL));
          constr_hex.assign(ibuf, 12);
        }

        const auto &vps_list = !cfg.all_vps.empty() ? cfg.all_vps : trk.hevc.sprop_vps;
        const auto &sps_list = !cfg.all_sps.empty() ? cfg.all_sps : trk.hevc.sprop_sps;
        const auto &pps_list = !cfg.all_pps.empty() ? cfg.all_pps : trk.hevc.sprop_pps;

        const std::string vps_b64 = join_base64_nals(vps_list, cfg.vps);
        const std::string sps_b64 = join_base64_nals(sps_list, cfg.sps);
        const std::string pps_b64 = join_base64_nals(pps_list, cfg.pps);

        std::string fmtp;
        auto        append_kv = [&](std::string_view k, std::string_view v) {
          if (v.empty()) return;
          if (!fmtp.empty()) fmtp += ';';
          fmtp.append(k);
          fmtp.push_back('=');
          fmtp.append(v);
        };

        if (have_ptl) {
          append_kv("profile-space", std::to_string(pspace));
          append_kv("profile-id", std::to_string(pid));
          append_kv("tier-flag", std::to_string(tflag));
          append_kv("level-id", std::to_string(lid));
        }
        if (!compat_hex.empty()) {
          append_kv("profile-compatibility-indicator", compat_hex);
        }
        if (!constr_hex.empty()) {
          append_kv("interop-constraints", constr_hex);
        }
        if (trk.hevc.sprop_max_don_diff > 0U) {
          append_kv("sprop-max-don-diff", std::to_string(trk.hevc.sprop_max_don_diff));
        }
        append_kv("sprop-vps", vps_b64);
        append_kv("sprop-sps", sps_b64);
        append_kv("sprop-pps", pps_b64);

        if (!fmtp.empty()) {
          sdp += "a=fmtp:" + pt_str + " " + fmtp + "\r\n";
        }
      } else if (trk.kind == media::MediaKind::Video && trk.codec == media::VideoCodec::H264) {
        /// RFC 6184 §8.1 Declarative H.264 fmtp
        std::string       plid     = cfg.has_h264_profile ? cfg.h264_profile_level_id
            : trk.h264.has_explicit_profile               ? trk.h264.profile_level_id
                                                          : "42e01f";
        const auto       &sps_list = !cfg.all_sps.empty() ? cfg.all_sps : trk.h264.sprop_sps;
        const auto       &pps_list = !cfg.all_pps.empty() ? cfg.all_pps : trk.h264.sprop_pps;
        const std::string sps_b64  = join_base64_nals(sps_list, cfg.sps);
        const std::string pps_b64  = join_base64_nals(pps_list, cfg.pps);

        sdp += "a=fmtp:" + pt_str +
            " packetization-mode=" + std::to_string(trk.h264.packetization_mode) +
            ";profile-level-id=" + plid;
        if (!sps_b64.empty() || !pps_b64.empty()) {
          sdp += ";sprop-parameter-sets=" + sps_b64;
          if (!sps_b64.empty() && !pps_b64.empty()) sdp.push_back(',');
          sdp += pps_b64;
        }
        sdp += "\r\n";
      } else if (!trk.fmtp_raw.empty()) {
        sdp += "a=fmtp:" + pt_str + " " + trk.fmtp_raw + "\r\n";
      }

      for (const auto &attr : trk.passthrough_attrs) {
        sdp += "a=" + attr + "\r\n";
      }

      sdp += "a=control:trackID=" + std::to_string(trk.index) + "\r\n";
    }

    return sdp;
  }

  /// Convenience single-track synthesizer retained for standalone unit tests (updated with `c=`
  /// and SPS-derived `profile-level-id` / HEVC PTL).
  [[nodiscard]] std::string synthesize_client_sdp(bool is_hevc,
      const std::string                               &sps_b64,
      const std::string                               &pps_b64,
      const std::string                               &vps_b64 = "") const {
    media::StreamDescriptor desc{};
    media::TrackDescriptor  trk{};
    trk.index         = 0;
    trk.kind          = media::MediaKind::Video;
    trk.payload_type  = 96;
    trk.clock_rate    = 90000;
    trk.codec         = is_hevc ? media::VideoCodec::H265 : media::VideoCodec::H264;
    trk.encoding_name = is_hevc ? "H265" : "H264";
    desc.tracks.push_back(std::move(trk));

    media::ConfigTracker tracker;
    tracker.set_codec(is_hevc ? media::VideoCodec::H265 : media::VideoCodec::H264);
    if (is_hevc && !vps_b64.empty()) {
      if (auto v = media::base64::decode(vps_b64); v && !v->empty()) {
        tracker.stage_vps(v->data(), v->size());
      }
    }
    if (!sps_b64.empty()) {
      if (auto s = media::base64::decode(sps_b64); s && !s->empty()) {
        tracker.stage_sps(s->data(), s->size());
      }
    }
    if (!pps_b64.empty()) {
      if (auto p = media::base64::decode(pps_b64); p && !p->empty()) {
        tracker.stage_pps(p->data(), p->size());
      }
    }
    (void)tracker.commit_au();
    auto snap = tracker.current_snapshot();
    return render_sdp(desc, *snap);
  }
};

} //< namespace rivet::proto::rtsp
