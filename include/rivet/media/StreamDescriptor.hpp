#pragma once

#include "Base64.hpp"
#include "HevcPayload.hpp"
#include "rivet/config/RivetConfig.hpp"
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::media {

enum class VideoCodec : uint8_t { Unknown, H264, H265 };
enum class MediaKind : uint8_t { Video, Audio, Application };
enum class TxMode : uint8_t { SRST, MRST, MRMT };

/// RFC 7798 §7.1 HEVC format parameters; defaults match RFC-inferred declarative values.
struct HevcFmtp {
  uint8_t                           profile_space{0};
  uint8_t                           profile_id{1};
  uint8_t                           tier_flag{0};
  uint8_t                           level_id{93};
  bool                              has_explicit_ptl{false};
  TxMode                            tx_mode{TxMode::SRST};
  uint16_t                          sprop_max_don_diff{0};
  std::vector<std::vector<uint8_t>> sprop_vps;
  std::vector<std::vector<uint8_t>> sprop_sps;
  std::vector<std::vector<uint8_t>> sprop_pps;
  std::string                       profile_compatibility_indicator;
  std::string                       interop_constraints;
};

struct H264Fmtp {
  uint8_t                           packetization_mode{1};
  std::string                       profile_level_id{"42e01f"};
  bool                              has_explicit_profile{false};
  std::vector<std::vector<uint8_t>> sprop_sps;
  std::vector<std::vector<uint8_t>> sprop_pps;
};

struct TrackDescriptor {
  uint8_t                  index{0}; //< 0..7; packed into PacketRef byte 30
  MediaKind                kind{MediaKind::Video};
  uint8_t                  payload_type{96};
  uint32_t                 clock_rate{90000};
  uint8_t                  channels{1};
  VideoCodec               codec{VideoCodec::Unknown};
  std::string              encoding_name;
  std::string              control; //< Raw `a=control:` value from publisher SDP
  std::string              fmtp_raw;
  HevcFmtp                 hevc;
  H264Fmtp                 h264;
  std::vector<std::string> passthrough_attrs;
};

struct StreamDescriptor {
  static constexpr size_t MAX_SDP_BYTES  = 16384;
  static constexpr size_t MAX_LINE_BYTES = 1024;
  static constexpr size_t MAX_TRACKS     = 8;

  std::string                  session_name{"RIVET"};
  std::string                  session_control{"*"};
  std::vector<TrackDescriptor> tracks;
  uint32_t                     version{1};

  [[nodiscard]] const TrackDescriptor *find_first_track(MediaKind k) const noexcept {
    for (const auto &t : tracks) {
      if (t.kind == k) return &t;
    }
    return nullptr;
  }

  [[nodiscard]] const TrackDescriptor *track_at(size_t idx) const noexcept {
    return (idx < tracks.size()) ? &tracks[idx] : nullptr;
  }
};

enum class SdpError : uint8_t {
  None,
  TooLarge,
  Malformed,
  NoMedia,
  UnsupportedCodec,
  UnsupportedTxMode,
  TooManyTracks
};

struct SdpParseResult {
  std::optional<StreamDescriptor> descriptor;
  SdpError                        error{SdpError::None};
};

namespace sdp_detail {

[[nodiscard]] inline std::string_view trim_ows(std::string_view sv) noexcept {
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
  return sv;
}

[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 32) : a[i];
    char cb = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 32) : b[i];
    if (ca != cb) return false;
  }
  return true;
}

[[nodiscard]] inline bool is_safe_printable_ascii(std::string_view sv) noexcept {
  for (unsigned char c : sv) {
    if (c < 0x20U || c == 0x7FU) {
      return false; //< Rejects CR, LF, NUL, and control chars
    }
  }
  return true;
}

[[nodiscard]] inline bool is_hex_string(std::string_view sv) noexcept {
  if (sv.empty()) return false;
  for (unsigned char c : sv) {
    if (!std::isxdigit(c)) return false;
  }
  return true;
}

inline bool decode_nal_csv(std::string_view csv,
    uint8_t                                 expected_hevc_type,
    std::vector<std::vector<uint8_t>>      &out) {
  size_t pos = 0;
  while (pos <= csv.size()) {
    const size_t comma = csv.find(',', pos);
    const auto   token = trim_ows(
        (comma == std::string_view::npos) ? csv.substr(pos) : csv.substr(pos, comma - pos));
    if (!token.empty()) {
      auto decoded = base64::decode(token);
      if (!decoded || decoded->size() < 2U) {
        return false;
      }
      const auto hdr = hevc::parse_nal_header((*decoded)[0], (*decoded)[1]);
      if (!hdr.ok || hdr.type != expected_hevc_type) {
        return false;
      }
      out.push_back(std::move(*decoded));
    }
    if (comma == std::string_view::npos) break;
    pos = comma + 1U;
  }
  return true;
}

inline bool parse_fmtp_params(TrackDescriptor &track, std::string_view params) {
  if (!is_safe_printable_ascii(params)) {
    return false;
  }
  track.fmtp_raw.assign(params);

  size_t pos = 0;
  while (pos < params.size()) {
    const size_t semi = params.find(';', pos);
    const auto   item = trim_ows(
        (semi == std::string_view::npos) ? params.substr(pos) : params.substr(pos, semi - pos));
    pos = (semi == std::string_view::npos) ? params.size() : (semi + 1U);
    if (item.empty()) continue;

    const size_t eq  = item.find('=');
    const auto   key = trim_ows((eq == std::string_view::npos) ? item : item.substr(0, eq));
    const auto   val =
        trim_ows((eq == std::string_view::npos) ? std::string_view{} : item.substr(eq + 1U));

    /// Codec-specific attributes may arrive before or after `a=rtpmap`; check keys cleanly.
    if (iequals(key, "tx-mode")) {
      if (iequals(val, "SRST")) {
        track.hevc.tx_mode = TxMode::SRST;
      } else if (iequals(val, "MRST")) {
        track.hevc.tx_mode = TxMode::MRST;
      } else {
        track.hevc.tx_mode = TxMode::MRMT;
      }
    } else if (iequals(key, "profile-space")) {
      auto v = config::parse_uint<uint8_t>(val, 0, 3);
      if (!v) return false;
      track.hevc.profile_space    = *v;
      track.hevc.has_explicit_ptl = true;
    } else if (iequals(key, "profile-id")) {
      auto v = config::parse_uint<uint8_t>(val, 0, 31);
      if (!v) return false;
      track.hevc.profile_id       = *v;
      track.hevc.has_explicit_ptl = true;
    } else if (iequals(key, "tier-flag")) {
      auto v = config::parse_uint<uint8_t>(val, 0, 1);
      if (!v) return false;
      track.hevc.tier_flag        = *v;
      track.hevc.has_explicit_ptl = true;
    } else if (iequals(key, "level-id")) {
      auto v = config::parse_uint<uint8_t>(val, 0, 255);
      if (!v) return false;
      track.hevc.level_id         = *v;
      track.hevc.has_explicit_ptl = true;
    } else if (iequals(key, "sprop-max-don-diff")) {
      auto v = config::parse_uint<uint16_t>(val, 0, 32767);
      if (!v) return false;
      track.hevc.sprop_max_don_diff = *v;
    } else if (iequals(key, "profile-compatibility-indicator")) {
      if (!is_hex_string(val) || val.size() > 8U) return false;
      track.hevc.profile_compatibility_indicator.assign(val);
    } else if (iequals(key, "interop-constraints")) {
      if (!is_hex_string(val) || val.size() > 12U) return false;
      track.hevc.interop_constraints.assign(val);
    } else if (iequals(key, "sprop-vps")) {
      if (!decode_nal_csv(val, 32U, track.hevc.sprop_vps)) return false;
    } else if (iequals(key, "sprop-sps")) {
      if (!decode_nal_csv(val, 33U, track.hevc.sprop_sps)) return false;
    } else if (iequals(key, "sprop-pps")) {
      if (!decode_nal_csv(val, 34U, track.hevc.sprop_pps)) return false;
    } else if (iequals(key, "profile-level-id")) {
      if (val.size() == 6U && is_hex_string(val)) {
        track.h264.profile_level_id     = std::string(val);
        track.h264.has_explicit_profile = true;
      }
    } else if (iequals(key, "packetization-mode")) {
      if (auto v = config::parse_uint<uint8_t>(val, 0, 2)) {
        track.h264.packetization_mode = *v;
      }
    } else if (iequals(key, "sprop-parameter-sets")) {
      size_t cpos = 0;
      while (cpos <= val.size()) {
        const size_t comma = val.find(',', cpos);
        const auto   tok   = trim_ows(
            (comma == std::string_view::npos) ? val.substr(cpos) : val.substr(cpos, comma - cpos));
        if (!tok.empty()) {
          auto decoded = base64::decode(tok);
          if (!decoded || decoded->empty() || ((*decoded)[0] & 0x80U) != 0U) {
            return false;
          }
          const uint8_t ntype = (*decoded)[0] & 0x1FU;
          if (ntype == 7U) {
            track.h264.sprop_sps.push_back(std::move(*decoded));
          } else if (ntype == 8U) {
            track.h264.sprop_pps.push_back(std::move(*decoded));
          }
        }
        if (comma == std::string_view::npos) break;
        cpos = comma + 1U;
      }
    }
  }
  return true;
}

} //< namespace sdp_detail

/// Parses and validates an RTSP ANNOUNCE SDP body according to RFC 8866, RFC 6184, and RFC 7798.
[[nodiscard]] inline SdpParseResult parse_announce_sdp(std::string_view sdp) {
  if (sdp.size() > StreamDescriptor::MAX_SDP_BYTES) {
    return {std::nullopt, SdpError::TooLarge};
  }
  if (sdp.empty()) {
    return {std::nullopt, SdpError::Malformed};
  }

  StreamDescriptor desc{};
  bool             saw_version = false;
  TrackDescriptor *cur_track   = nullptr;

  /// Deferred fmtp lines when `a=fmtp:` precedes `a=rtpmap:` within an `m=` block
  struct DeferredFmtp {
    size_t      track_idx;
    uint8_t     pt;
    std::string params;
  };
  std::vector<DeferredFmtp> deferred_fmtps;

  size_t line_start = 0;
  while (line_start <= sdp.size()) {
    const size_t     nl   = sdp.find('\n', line_start);
    std::string_view line = (nl == std::string_view::npos)
        ? sdp.substr(line_start)
        : sdp.substr(line_start, nl - line_start);
    line_start            = (nl == std::string_view::npos) ? (sdp.size() + 1U) : (nl + 1U);

    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.size() > StreamDescriptor::MAX_LINE_BYTES) {
      return {std::nullopt, SdpError::Malformed};
    }
    if (line.empty()) continue;

    if (line.size() < 2U || line[1] != '=' || !sdp_detail::is_safe_printable_ascii(line)) {
      return {std::nullopt, SdpError::Malformed};
    }

    const char             type = line[0];
    const std::string_view val  = line.substr(2);

    if (type == 'v') {
      if (sdp_detail::trim_ows(val) != "0") {
        return {std::nullopt, SdpError::Malformed};
      }
      saw_version = true;
    } else if (type == 's') {
      desc.session_name.assign(val.empty() ? std::string_view("RIVET") : val);
    } else if (type == 'm') {
      /// `m=<media> <port> <proto> <fmt> ...`
      if (desc.tracks.size() >= StreamDescriptor::MAX_TRACKS) {
        return {std::nullopt, SdpError::TooManyTracks};
      }

      std::vector<std::string_view> toks;
      size_t                        p = 0;
      while (p < val.size()) {
        while (p < val.size() && (val[p] == ' ' || val[p] == '\t')) ++p;
        if (p >= val.size()) break;
        size_t e = p;
        while (e < val.size() && val[e] != ' ' && val[e] != '\t') ++e;
        toks.push_back(val.substr(p, e - p));
        p = e;
      }
      if (toks.size() < 4U) {
        return {std::nullopt, SdpError::Malformed};
      }

      TrackDescriptor track{};
      track.index = static_cast<uint8_t>(desc.tracks.size());
      if (sdp_detail::iequals(toks[0], "video")) {
        track.kind       = MediaKind::Video;
        track.clock_rate = 90000;
      } else if (sdp_detail::iequals(toks[0], "audio")) {
        track.kind       = MediaKind::Audio;
        track.clock_rate = 48000;
      } else {
        track.kind       = MediaKind::Application;
        track.clock_rate = 90000;
      }

      /// Validate RTP transport profile (RTP/AVP or RTP/AVPF)
      if (toks[2].find("RTP/AVP") == std::string_view::npos) {
        return {std::nullopt, SdpError::UnsupportedTxMode};
      }

      auto pt = config::parse_uint<uint8_t>(toks[3], 0, 127);
      if (!pt) {
        return {std::nullopt, SdpError::Malformed};
      }
      track.payload_type = *pt;

      desc.tracks.push_back(std::move(track));
      cur_track = &desc.tracks.back();
    } else if (type == 'a') {
      if (!cur_track) {
        if (val.starts_with("control:")) {
          desc.session_control.assign(sdp_detail::trim_ows(val.substr(8)));
        }
        continue;
      }

      if (val.starts_with("control:")) {
        cur_track->control.assign(sdp_detail::trim_ows(val.substr(8)));
      } else if (val.starts_with("rtpmap:")) {
        auto       rest = sdp_detail::trim_ows(val.substr(7));
        const auto sp   = rest.find_first_of(" \t");
        if (sp == std::string_view::npos) {
          return {std::nullopt, SdpError::Malformed};
        }
        auto pt = config::parse_uint<uint8_t>(rest.substr(0, sp), 0, 127);
        if (!pt) {
          return {std::nullopt, SdpError::Malformed};
        }
        if (*pt != cur_track->payload_type) {
          continue; //< Ignore secondary format offerings on the same `m=` line
        }
        auto       enc_spec = sdp_detail::trim_ows(rest.substr(sp + 1));
        const auto sl1      = enc_spec.find('/');
        if (sl1 == std::string_view::npos) {
          return {std::nullopt, SdpError::Malformed};
        }
        const auto enc_name  = sdp_detail::trim_ows(enc_spec.substr(0, sl1));
        auto       rate_part = enc_spec.substr(sl1 + 1);
        const auto sl2       = rate_part.find('/');
        if (sl2 != std::string_view::npos) {
          auto ch = config::parse_uint<uint8_t>(rate_part.substr(sl2 + 1), 1, 16);
          if (ch) cur_track->channels = *ch;
          rate_part = rate_part.substr(0, sl2);
        }
        auto clk = config::parse_uint<uint32_t>(rate_part, 1000, 10000000);
        if (!clk || enc_name.empty()) {
          return {std::nullopt, SdpError::Malformed};
        }
        cur_track->encoding_name.assign(enc_name);
        cur_track->clock_rate = *clk;

        if (cur_track->kind == MediaKind::Video) {
          if (sdp_detail::iequals(enc_name, "H265") || sdp_detail::iequals(enc_name, "HEVC")) {
            cur_track->codec = VideoCodec::H265;
          } else if (sdp_detail::iequals(enc_name, "H264")) {
            cur_track->codec = VideoCodec::H264;
          } else {
            cur_track->codec = VideoCodec::Unknown;
          }
        }
      } else if (val.starts_with("fmtp:")) {
        auto       rest = sdp_detail::trim_ows(val.substr(5));
        const auto sp   = rest.find_first_of(" \t");
        if (sp != std::string_view::npos) {
          auto pt = config::parse_uint<uint8_t>(rest.substr(0, sp), 0, 127);
          if (!pt) return {std::nullopt, SdpError::Malformed};
          deferred_fmtps.push_back(
              {cur_track->index, *pt, std::string(sdp_detail::trim_ows(rest.substr(sp + 1)))});
        }
      } else if (val.starts_with("ssrc:")) {
        /// RFC 5576 source-level fmtp: `a=ssrc:<id> fmtp:<pt> <params>`
        const auto fp = val.find("fmtp:");
        if (fp != std::string_view::npos) {
          auto       rest = sdp_detail::trim_ows(val.substr(fp + 5));
          const auto sp   = rest.find_first_of(" \t");
          if (sp != std::string_view::npos) {
            if (auto pt = config::parse_uint<uint8_t>(rest.substr(0, sp), 0, 127)) {
              deferred_fmtps.push_back(
                  {cur_track->index, *pt, std::string(sdp_detail::trim_ows(rest.substr(sp + 1)))});
            }
          }
        }
      } else if (val.starts_with("framerate:") || val.starts_with("framesize:")) {
        cur_track->passthrough_attrs.emplace_back(val);
      }
    }
  }

  if (!saw_version) {
    return {std::nullopt, SdpError::Malformed};
  }
  if (desc.tracks.empty()) {
    return {std::nullopt, SdpError::NoMedia};
  }

  for (const auto &df : deferred_fmtps) {
    auto &trk = desc.tracks[df.track_idx];
    if (df.pt != trk.payload_type) continue;
    if (!sdp_detail::parse_fmtp_params(trk, df.params)) {
      return {std::nullopt, SdpError::Malformed};
    }
  }

  for (auto &trk : desc.tracks) {
    if (trk.encoding_name.empty()) {
      /// Static payload type fallback if no `a=rtpmap` was present
      if (trk.payload_type == 0U) {
        trk.encoding_name = "PCMU";
        trk.clock_rate    = 8000;
      } else if (trk.payload_type == 8U) {
        trk.encoding_name = "PCMA";
        trk.clock_rate    = 8000;
      }
    }
    if (trk.kind == MediaKind::Video && trk.codec == VideoCodec::Unknown) {
      return {std::nullopt, SdpError::UnsupportedCodec};
    }
    if (trk.codec == VideoCodec::H265 && trk.hevc.tx_mode != TxMode::SRST) {
      return {std::nullopt, SdpError::UnsupportedTxMode};
    }
  }

  return {std::move(desc), SdpError::None};
}

/// Resolves an RTSP SETUP request URL against the stream's base path and track `a=control:`
/// attributes (plus RIVET's canonical `trackID=<index>` control URIs).
[[nodiscard]] inline std::optional<uint8_t> resolve_setup_track(const StreamDescriptor &desc,
    std::string_view                                                                    stream_path,
    std::string_view request_url) noexcept {
  /// Strip scheme and authority (`rtsp://host:port/...` -> `/...`)
  std::string_view path = request_url;
  if (const auto scheme = path.find("://"); scheme != std::string_view::npos) {
    path.remove_prefix(scheme + 3U);
    if (const auto slash = path.find('/'); slash != std::string_view::npos) {
      path.remove_prefix(slash);
    } else {
      path = "/";
    }
  }
  /// Strip query string if any
  if (const auto q = path.find('?'); q != std::string_view::npos) {
    path = path.substr(0, q);
  }
  while (!path.empty() && path.front() == '/') path.remove_prefix(1);
  while (!path.empty() && path.back() == '/') path.remove_suffix(1);

  std::string_view clean_stream = stream_path;
  while (!clean_stream.empty() && clean_stream.front() == '/') clean_stream.remove_prefix(1);
  while (!clean_stream.empty() && clean_stream.back() == '/') clean_stream.remove_suffix(1);

  /// Extract relative suffix after `<stream_path>/` (or trailing segment when `stream_path` is unset)
  std::string_view suffix;
  if (!clean_stream.empty()) {
    if (!path.starts_with(clean_stream)) {
      return std::nullopt;
    }
    suffix = path.substr(clean_stream.size());
    if (!suffix.empty() && suffix.front() != '/') {
      return std::nullopt;
    }
    while (!suffix.empty() && suffix.front() == '/') suffix.remove_prefix(1);
  } else {
    const auto sl = path.rfind('/');
    suffix        = (sl == std::string_view::npos) ? path : path.substr(sl + 1U);
  }

  /// Single-track stream SETUP directly against the aggregate URL
  if (suffix.empty()) {
    if (desc.tracks.size() == 1U) {
      return uint8_t{0};
    }
    return std::nullopt;
  }

  /// 1. Check RIVET's canonical `trackID=<index>` control token
  if (suffix.starts_with("trackID=")) {
    if (auto idx = config::parse_uint<uint8_t>(suffix.substr(8), 0, 7)) {
      if (*idx < desc.tracks.size()) {
        return *idx;
      }
    }
  }

  /// 2. Match against publisher's `a=control:` attribute (handles relative or full URLs)
  for (const auto &trk : desc.tracks) {
    if (trk.control.empty()) continue;
    std::string_view ctrl = trk.control;
    if (const auto sl = ctrl.rfind('/'); sl != std::string_view::npos) {
      const auto tail = ctrl.substr(sl + 1U);
      if (tail == suffix || ctrl == request_url) {
        return trk.index;
      }
    } else if (ctrl == suffix) {
      return trk.index;
    }
  }

  /// 3. Compatibility check for `streamid=<idx>` (`0..N-1`)
  if (suffix.starts_with("streamid=")) {
    if (auto idx = config::parse_uint<uint8_t>(suffix.substr(9), 0, 7)) {
      if (*idx < desc.tracks.size()) {
        return *idx;
      }
    }
  }

  return std::nullopt;
}

} //< namespace rivet::media
