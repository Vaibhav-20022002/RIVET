#pragma once

#include "RtspDigestAuth.hpp"
#include "SdpNegotiator.hpp"
#include "rivet/config/RivetConfig.hpp"
#include "rivet/media/ConfigGeneration.hpp"
#include "rivet/media/StreamDescriptor.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::proto::rtsp {

/// The state of the RTSP connection (RFC 2326 / RFC 7826)
enum class RtspState : uint8_t {
  Init,     //< Connected; OPTIONS / DESCRIBE / ANNOUNCE allowed
  Ready,    //< At least one track SETUP completed
  Playing,  //< Viewer PLAY active
  Recording //< Publisher RECORD active (sole state allowed to push media!)
};

/// Per-channel interleaved demultiplexing entry (`$` channel 0..255 -> track & RTP/RTCP plane).
struct InterleavedChannelBinding {
  bool    bound{false};
  uint8_t track_index{0};
  bool    is_rtcp{false};
};

/// Per-track negotiated transport descriptor on an RTSP session.
struct TrackTransportBinding {
  bool     is_setup{false};
  bool     is_interleaved{false};
  uint8_t  track_index{0};
  uint8_t  rtp_channel{0};
  uint8_t  rtcp_channel{1};
  uint16_t client_rtp_port{0};
  uint16_t client_rtcp_port{0};
  uint16_t server_rtp_port{0};
  uint16_t server_rtcp_port{0};
};

/// Optional `RTP-Info` anchor per track supplied when generating a `PLAY` 200 OK response.
struct PlayTrackRtpInfo {
  uint8_t     track_index{0};
  std::string track_url;
  bool        has_seq_and_ts{false};
  uint16_t    seq{0};
  uint32_t    rtptime{0};
};

/// Helper extracting request method, URI, and case-insensitive headers from raw RTSP text.
struct ParsedRtspRequest {
  bool             valid{false};
  std::string_view method;
  std::string_view uri;
  std::string_view version;
  std::string_view cseq;
  std::string_view session;
  std::string_view transport;
  std::string_view authorization;
  std::string_view body;
};

[[nodiscard]] inline ParsedRtspRequest parse_rtsp_request_text(std::string_view raw) noexcept {
  ParsedRtspRequest out{};
  const auto        hdr_end = raw.find("\r\n\r\n");
  const auto        headers = (hdr_end == std::string_view::npos) ? raw : raw.substr(0, hdr_end);
  if (hdr_end != std::string_view::npos) {
    out.body = raw.substr(hdr_end + 4U);
  }

  const auto line_end = headers.find("\r\n");
  const auto req_line =
      (line_end == std::string_view::npos) ? headers : headers.substr(0, line_end);

  const auto sp1 = req_line.find(' ');
  if (sp1 == std::string_view::npos || sp1 == 0U) return out;
  const auto sp2 = req_line.find(' ', sp1 + 1U);
  if (sp2 == std::string_view::npos || sp2 == sp1 + 1U) return out;

  out.method  = req_line.substr(0, sp1);
  out.uri     = req_line.substr(sp1 + 1U, sp2 - (sp1 + 1U));
  out.version = media::sdp_detail::trim_ows(req_line.substr(sp2 + 1U));
  if (!out.version.starts_with("RTSP/1.")) {
    return out;
  }

  if (line_end != std::string_view::npos) {
    size_t pos = line_end + 2U;
    while (pos < headers.size()) {
      const auto next = headers.find("\r\n", pos);
      const auto hl =
          (next == std::string_view::npos) ? headers.substr(pos) : headers.substr(pos, next - pos);
      pos = (next == std::string_view::npos) ? headers.size() : (next + 2U);
      if (hl.empty()) continue;

      const auto colon = hl.find(':');
      if (colon == std::string_view::npos) return out;
      const auto k = media::sdp_detail::trim_ows(hl.substr(0, colon));
      const auto v = media::sdp_detail::trim_ows(hl.substr(colon + 1U));

      if (media::sdp_detail::iequals(k, "CSeq")) {
        out.cseq = v;
      } else if (media::sdp_detail::iequals(k, "Session")) {
        /// Strip optional `;timeout=...` parameter from Session header
        const auto semi = v.find(';');
        out.session =
            media::sdp_detail::trim_ows((semi == std::string_view::npos) ? v : v.substr(0, semi));
      } else if (media::sdp_detail::iequals(k, "Transport")) {
        out.transport = v;
      } else if (media::sdp_detail::iequals(k, "Authorization")) {
        out.authorization = v;
      }
    }
  }

  /// RFC 2326 §10 / RFC 7826: CSeq is mandatory and must be a valid unsigned integer
  if (out.cseq.empty() || !config::parse_uint<uint32_t>(out.cseq).has_value()) {
    return out;
  }

  out.valid = true;
  return out;
}

/// Manages the RTSP 1.0 control plane for a single subscriber or publisher session.
class RtspControlSession {
  RtspState   state_{RtspState::Init};
  std::string session_id_; //< Allocated via 64-bit CSPRNG on first SETUP
  std::string auth_nonce_; //< Per-session Digest Auth challenge nonce
  std::string auth_user_;
  std::string auth_password_;
  std::string stream_path_;

  /// Stream metadata: populated either by ANNOUNCE (publisher) or attached from worker (viewer)
  std::optional<media::StreamDescriptor>         stream_descriptor_;
  std::shared_ptr<const media::ConfigGeneration> active_config_snapshot_;
  std::string                                    rendered_sdp_;
  uint32_t                                       advertised_generation_{0};
  bool                                           has_external_publisher_conflict_{false};

  bool     is_record_{false};
  bool     has_announced_{false};
  bool     publish_authenticated_{false};
  bool     has_explicit_server_ports_{false};
  bool     is_interleaved_{false};
  uint16_t client_rtp_port_{0};
  uint16_t client_rtcp_port_{0};
  uint16_t base_rtp_port_{20000};
  uint16_t base_rtcp_port_{20001};
  uint16_t server_rtp_port_{20000};
  uint16_t server_rtcp_port_{20001};
  uint8_t  interleaved_channel_{0};
  uint8_t  last_setup_track_{0};
  uint32_t timeout_sec_{60};

  std::array<TrackTransportBinding, media::StreamDescriptor::MAX_TRACKS> track_bindings_{};
  std::array<InterleavedChannelBinding, 256>                             channel_map_{};
  std::vector<PlayTrackRtpInfo>                                          play_rtp_info_;

  [[nodiscard]] static std::string format_error(int code,
      std::string_view                              reason,
      std::string_view                              cseq,
      std::string_view                              extra_headers = "") {
    std::string resp = "RTSP/1.0 " + std::to_string(code) + " ";
    resp.append(reason);
    resp += "\r\n";
    if (!cseq.empty()) {
      resp += "CSeq: ";
      resp.append(cseq);
      resp += "\r\n";
    }
    if (!extra_headers.empty()) {
      resp.append(extra_headers);
    }
    resp += "\r\n";
    return resp;
  }

  [[nodiscard]] bool check_publish_auth(const ParsedRtspRequest &req, std::string &err_out) {
    if (auth_user_.empty()) {
      return true;
    }
    if (publish_authenticated_ && req.authorization.empty()) {
      return true;
    }
    if (auth_nonce_.empty()) {
      auth_nonce_ = generate_random_hex_id();
    }
    if (verify_rtsp_digest_auth(
            req.authorization, req.method, "RIVET", auth_nonce_, auth_user_, auth_password_)) {
      publish_authenticated_ = true;
      return true;
    }
    const std::string www =
        "WWW-Authenticate: Digest realm=\"RIVET\", nonce=\"" + auth_nonce_ + "\"\r\n";
    err_out = format_error(401, "Unauthorized", req.cseq, www);
    return false;
  }

public:
  explicit RtspControlSession(uint16_t rtp_port = 20000, uint16_t rtcp_port = 20001)
      : auth_nonce_(generate_random_hex_id())
      , base_rtp_port_(rtp_port)
      , base_rtcp_port_(rtcp_port)
      , server_rtp_port_(rtp_port)
      , server_rtcp_port_(rtcp_port) {}

  void set_base_ports(uint16_t rtp, uint16_t rtcp) noexcept {
    base_rtp_port_             = rtp;
    base_rtcp_port_            = rtcp;
    server_rtp_port_           = rtp;
    server_rtcp_port_          = rtcp;
    has_explicit_server_ports_ = false;
  }

  void set_stream_path(std::string path) { stream_path_ = std::move(path); }

  void set_publish_credentials(std::string user, std::string pass) {
    auth_user_     = std::move(user);
    auth_password_ = std::move(pass);
  }

  void set_auth_nonce(std::string nonce) {
    if (!nonce.empty()) {
      auth_nonce_ = std::move(nonce);
    }
  }

  [[nodiscard]] const std::string &get_auth_nonce() const noexcept { return auth_nonce_; }

  void set_session_timeout_seconds(uint32_t sec) noexcept { timeout_sec_ = (sec > 0U) ? sec : 60U; }

  /// Attaches live stream descriptor & active ConfigGeneration snapshot before DESCRIBE / SETUP.
  void set_stream_context(const std::optional<media::StreamDescriptor> &desc,
      std::shared_ptr<const media::ConfigGeneration>                    cfg_snap,
      bool external_publisher_active = false) {
    stream_descriptor_               = desc;
    active_config_snapshot_          = std::move(cfg_snap);
    has_external_publisher_conflict_ = external_publisher_active;
    if (stream_descriptor_ && active_config_snapshot_) {
      rendered_sdp_ = SdpNegotiator::render_sdp(*stream_descriptor_, *active_config_snapshot_);
    } else if (stream_descriptor_) {
      media::ConfigGeneration empty_cfg{};
      rendered_sdp_ = SdpNegotiator::render_sdp(*stream_descriptor_, empty_cfg);
    } else {
      rendered_sdp_.clear();
    }
  }

  /// Legacy test helper: parses `sdp` into `StreamDescriptor` + seeds Generation 1 so viewer
  /// `DESCRIBE`/`SETUP` unit tests have a valid published stream.
  void set_announced_sdp(std::string sdp) {
    auto parsed = media::parse_announce_sdp(sdp);
    if (parsed.descriptor.has_value()) {
      media::ConfigTracker tracker;
      (void)tracker.seed_from_descriptor(*parsed.descriptor);
      stream_descriptor_      = std::move(parsed.descriptor);
      active_config_snapshot_ = tracker.current_snapshot();
      rendered_sdp_ = SdpNegotiator::render_sdp(*stream_descriptor_, *active_config_snapshot_);
    } else {
      stream_descriptor_.reset();
      rendered_sdp_.clear();
    }
  }

  void set_play_rtp_info(std::vector<PlayTrackRtpInfo> info) { play_rtp_info_ = std::move(info); }

  /// Handles incoming RTSP requests and generates the appropriate RFC 2326 / RFC 7826 response.
  std::string handle_request(std::string_view request) {
    const ParsedRtspRequest req = parse_rtsp_request_text(request);
    if (!req.valid) {
      return format_error(400, "Bad Request", req.cseq);
    }

    if (req.method == "OPTIONS") {
      return handle_options(req);
    }
    if (req.method == "GET_PARAMETER") {
      return handle_get_parameter(req);
    }
    if (req.method == "ANNOUNCE") {
      return handle_announce(req);
    }
    if (req.method == "DESCRIBE") {
      return handle_describe(req);
    }
    if (req.method == "SETUP") {
      return handle_setup(req);
    }
    if (req.method == "PLAY") {
      return handle_play(req);
    }
    if (req.method == "RECORD") {
      return handle_record(req);
    }
    if (req.method == "TEARDOWN") {
      return handle_teardown(req);
    }

    /// RFC 2326 §7.1.1 / RFC 7826 §17.5.2: Unknown or unimplemented methods (including PAUSE) -> 501
    return format_error(501, "Not Implemented", req.cseq);
  }

  [[nodiscard]] RtspState          get_state() const noexcept { return state_; }
  [[nodiscard]] const std::string &get_session_id() const noexcept { return session_id_; }
  [[nodiscard]] bool               is_record_mode() const noexcept { return is_record_; }
  [[nodiscard]] bool               has_announced() const noexcept { return has_announced_; }
  [[nodiscard]] bool               is_interleaved_mode() const noexcept { return is_interleaved_; }
  [[nodiscard]] uint16_t           get_client_rtp_port() const noexcept { return client_rtp_port_; }
  [[nodiscard]] uint16_t get_client_rtcp_port() const noexcept { return client_rtcp_port_; }
  [[nodiscard]] uint8_t  get_interleaved_channel() const noexcept { return interleaved_channel_; }
  [[nodiscard]] uint8_t  get_last_setup_track() const noexcept { return last_setup_track_; }
  [[nodiscard]] uint32_t get_advertised_generation() const noexcept {
    return advertised_generation_;
  }
  [[nodiscard]] const std::string &get_announced_sdp() const noexcept { return rendered_sdp_; }
  [[nodiscard]] const std::optional<media::StreamDescriptor> &
  get_stream_descriptor() const noexcept {
    return stream_descriptor_;
  }

  [[nodiscard]] const TrackTransportBinding &track_binding(uint8_t idx) const noexcept {
    return track_bindings_[idx & 0x07U];
  }

  [[nodiscard]] InterleavedChannelBinding lookup_interleaved_channel(uint8_t ch) const noexcept {
    return channel_map_[ch];
  }

  void set_server_ports(uint16_t rtp, uint16_t rtcp) noexcept {
    server_rtp_port_           = rtp;
    server_rtcp_port_          = rtcp;
    has_explicit_server_ports_ = true;
  }

private:
  std::string handle_options(const ParsedRtspRequest &req) const {
    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        std::string(req.cseq) +
        "\r\n"
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, ANNOUNCE, RECORD, GET_PARAMETER\r\n\r\n";
  }

  std::string handle_get_parameter(const ParsedRtspRequest &req) const {
    if (!req.session.empty()) {
      if (session_id_.empty() || req.session != session_id_) {
        return format_error(454, "Session Not Found", req.cseq);
      }
    }
    std::string resp = "RTSP/1.0 200 OK\r\nCSeq: " + std::string(req.cseq) + "\r\n";
    if (!session_id_.empty()) {
      resp += "Session: " + session_id_ + "\r\n";
    }
    resp += "\r\n";
    return resp;
  }

  std::string handle_announce(const ParsedRtspRequest &req) {
    std::string auth_err;
    if (!check_publish_auth(req, auth_err)) {
      return auth_err;
    }

    /// Reject concurrent second publisher while another session is actively publishing
    if (has_external_publisher_conflict_ && !has_announced_) {
      return format_error(455, "Method Not Valid in This State", req.cseq);
    }

    if (req.body.size() > media::StreamDescriptor::MAX_SDP_BYTES) {
      return format_error(413, "Request Entity Too Large", req.cseq);
    }

    auto parsed = media::parse_announce_sdp(req.body);
    if (parsed.error == media::SdpError::TooLarge) {
      return format_error(413, "Request Entity Too Large", req.cseq);
    }
    if (parsed.error == media::SdpError::UnsupportedCodec ||
        parsed.error == media::SdpError::UnsupportedTxMode ||
        parsed.error == media::SdpError::TooManyTracks) {
      return format_error(415, "Unsupported Media Type", req.cseq);
    }
    if (parsed.error != media::SdpError::None || !parsed.descriptor.has_value()) {
      return format_error(400, "Bad Request", req.cseq);
    }

    if (stream_path_.empty()) {
      std::string_view p = req.uri;
      if (const auto sch = p.find("://<"); sch != std::string_view::npos) {
        p.remove_prefix(sch + 3U);
        p = (p.find('/') != std::string_view::npos) ? p.substr(p.find('/')) : std::string_view{};
      }
      if (const auto q = p.find('?'); q != std::string_view::npos) p = p.substr(0, q);
      while (!p.empty() && p.front() == '/') p.remove_prefix(1);
      while (!p.empty() && p.back() == '/') p.remove_suffix(1);
      stream_path_.assign(p);
    }

    if (stream_descriptor_.has_value()) {
      parsed.descriptor->version = stream_descriptor_->version + 1U;
    }

    media::ConfigTracker seed_tracker;
    (void)seed_tracker.seed_from_descriptor(*parsed.descriptor);
    active_config_snapshot_ = seed_tracker.current_snapshot();
    stream_descriptor_      = std::move(parsed.descriptor);
    rendered_sdp_  = SdpNegotiator::render_sdp(*stream_descriptor_, *active_config_snapshot_);
    has_announced_ = true;
    is_record_     = true;

    return "RTSP/1.0 200 OK\r\nCSeq: " + std::string(req.cseq) + "\r\n\r\n";
  }

  std::string handle_describe(const ParsedRtspRequest &req) {
    /// DESCRIBE with no published stream MUST return 404, never fake H.264 SDP!
    if (!stream_descriptor_.has_value() || rendered_sdp_.empty()) {
      return format_error(404, "Not Found", req.cseq);
    }

    if (active_config_snapshot_) {
      advertised_generation_ = active_config_snapshot_->generation_id;
    }

    std::string base_url(req.uri);
    if (!base_url.ends_with('/')) {
      base_url.push_back('/');
    }

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        std::string(req.cseq) + "\r\n" + "Content-Base: " + base_url + "\r\n" +
        "Content-Type: application/sdp\r\n"
        "Content-Length: " +
        std::to_string(rendered_sdp_.size()) + "\r\n\r\n" + rendered_sdp_;
  }

  std::string handle_setup(const ParsedRtspRequest &req) {
    if (req.transport.empty()) {
      return format_error(461, "Unsupported Transport", req.cseq);
    }

    /// Validate Session header if present or if session already established
    if (!req.session.empty()) {
      if (session_id_.empty() || req.session != session_id_) {
        return format_error(454, "Session Not Found", req.cseq);
      }
    }

    /// Determine record vs play transport mode
    bool     want_record = has_announced_;
    bool     want_tcp    = false;
    bool     want_udp    = false;
    bool     saw_mcast   = false;
    bool     has_inter   = false;
    bool     has_cport   = false;
    uint8_t  ch_rtp      = 0;
    uint8_t  ch_rtcp     = 1;
    uint16_t c_rtp       = 0;
    uint16_t c_rtcp      = 0;

    /// Parse semicolon-separated Transport header tokens strictly
    size_t pos = 0;
    while (pos < req.transport.size()) {
      const auto semi = req.transport.find(';', pos);
      const auto tok  = media::sdp_detail::trim_ows((semi == std::string_view::npos)
              ? req.transport.substr(pos)
              : req.transport.substr(pos, semi - pos));
      pos             = (semi == std::string_view::npos) ? req.transport.size() : (semi + 1U);
      if (tok.empty()) continue;

      if (media::sdp_detail::iequals(tok, "RTP/AVP/TCP")) {
        want_tcp = true;
      } else if (media::sdp_detail::iequals(tok, "RTP/AVP") ||
          media::sdp_detail::iequals(tok, "RTP/AVP/UDP")) {
        want_udp = true;
      } else if (media::sdp_detail::iequals(tok, "multicast")) {
        saw_mcast = true;
      } else if (media::sdp_detail::iequals(tok, "unicast")) {
        /// Allowed
      } else {
        const auto eq = tok.find('=');
        if (eq == std::string_view::npos) continue;
        const auto k = media::sdp_detail::trim_ows(tok.substr(0, eq));
        auto       v = media::sdp_detail::trim_ows(tok.substr(eq + 1U));
        if (v.size() >= 2U && v.front() == '"' && v.back() == '"') {
          v = v.substr(1U, v.size() - 2U);
        }

        if (media::sdp_detail::iequals(k, "mode")) {
          if (media::sdp_detail::iequals(v, "record") || media::sdp_detail::iequals(v, "receive")) {
            want_record = true;
          } else if (!media::sdp_detail::iequals(v, "play")) {
            return format_error(461, "Unsupported Transport", req.cseq);
          }
        } else if (media::sdp_detail::iequals(k, "interleaved")) {
          const auto dash = v.find('-');
          if (dash == std::string_view::npos) {
            return format_error(461, "Unsupported Transport", req.cseq);
          }
          auto c0 = config::parse_uint<uint8_t>(v.substr(0, dash), 0, 254);
          auto c1 = config::parse_uint<uint8_t>(v.substr(dash + 1U), 1, 255);
          if (!c0 || !c1 || *c1 != static_cast<uint8_t>(*c0 + 1U)) {
            return format_error(461, "Unsupported Transport", req.cseq);
          }
          ch_rtp    = *c0;
          ch_rtcp   = *c1;
          has_inter = true;
          want_tcp  = true;
        } else if (media::sdp_detail::iequals(k, "client_port")) {
          const auto dash = v.find('-');
          if (dash == std::string_view::npos) {
            return format_error(461, "Unsupported Transport", req.cseq);
          }
          auto p0 = config::parse_uint<uint16_t>(v.substr(0, dash), 1, 65534);
          auto p1 = config::parse_uint<uint16_t>(v.substr(dash + 1U), 2, 65535);
          if (!p0 || !p1) {
            return format_error(461, "Unsupported Transport", req.cseq);
          }
          c_rtp     = *p0;
          c_rtcp    = *p1;
          has_cport = true;
        }
      }
    }

    if (saw_mcast || (!want_tcp && !want_udp)) {
      return format_error(461, "Unsupported Transport", req.cseq);
    }
    if (want_tcp && !has_inter) {
      /// Default interleaved pair if client requested `RTP/AVP/TCP;unicast` without explicit channels
      ch_rtp    = 0;
      ch_rtcp   = 1;
      has_inter = true;
    }
    if (!want_tcp && !has_cport) {
      return format_error(461, "Unsupported Transport", req.cseq);
    }

    if (want_record) {
      std::string auth_err;
      if (!check_publish_auth(req, auth_err)) {
        return auth_err;
      }
      if (!has_announced_ || !stream_descriptor_.has_value()) {
        return format_error(455, "Method Not Valid in This State", req.cseq);
      }
    } else if (!stream_descriptor_.has_value()) {
      return format_error(404, "Not Found", req.cseq);
    }

    /// Resolve track index from URL against `StreamDescriptor`
    auto track_opt = media::resolve_setup_track(*stream_descriptor_, stream_path_, req.uri);
    if (!track_opt.has_value()) {
      return format_error(404, "Not Found", req.cseq);
    }
    const uint8_t track_idx = *track_opt;

    if (want_tcp) {
      /// Reject channel collisions across distinct tracks
      if ((channel_map_[ch_rtp].bound && channel_map_[ch_rtp].track_index != track_idx) ||
          (channel_map_[ch_rtcp].bound && channel_map_[ch_rtcp].track_index != track_idx)) {
        return format_error(461, "Unsupported Transport", req.cseq);
      }
      channel_map_[ch_rtp]  = {.bound = true, .track_index = track_idx, .is_rtcp = false};
      channel_map_[ch_rtcp] = {.bound = true, .track_index = track_idx, .is_rtcp = true};
    }

    if (session_id_.empty()) {
      session_id_ = generate_random_hex_id();
    }

    is_record_        = want_record;
    is_interleaved_   = want_tcp;
    last_setup_track_ = track_idx;

    const uint16_t s_rtp  = has_explicit_server_ports_
        ? server_rtp_port_
        : static_cast<uint16_t>(base_rtp_port_ + (track_idx * 2U));
    const uint16_t s_rtcp = has_explicit_server_ports_
        ? server_rtcp_port_
        : static_cast<uint16_t>(base_rtcp_port_ + (track_idx * 2U));
    server_rtp_port_      = s_rtp;
    server_rtcp_port_     = s_rtcp;

    auto &tb            = track_bindings_[track_idx];
    tb.is_setup         = true;
    tb.is_interleaved   = want_tcp;
    tb.track_index      = track_idx;
    tb.rtp_channel      = ch_rtp;
    tb.rtcp_channel     = ch_rtcp;
    tb.client_rtp_port  = c_rtp;
    tb.client_rtcp_port = c_rtcp;
    tb.server_rtp_port  = s_rtp;
    tb.server_rtcp_port = s_rtcp;

    if (want_tcp) {
      interleaved_channel_ = ch_rtp;
    } else {
      client_rtp_port_  = c_rtp;
      client_rtcp_port_ = c_rtcp;
    }

    if (state_ == RtspState::Init) {
      state_ = RtspState::Ready;
    }

    std::string transport_hdr;
    if (want_tcp) {
      transport_hdr = "Transport: RTP/AVP/TCP;unicast;interleaved=" + std::to_string(ch_rtp) + "-" +
          std::to_string(ch_rtcp);
    } else {
      transport_hdr = "Transport: RTP/AVP/UDP;unicast;client_port=" + std::to_string(c_rtp) + "-" +
          std::to_string(c_rtcp) + ";server_port=" + std::to_string(s_rtp) + "-" +
          std::to_string(s_rtcp);
    }
    if (is_record_) {
      transport_hdr += ";mode=record";
    }

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        std::string(req.cseq) + "\r\n" + transport_hdr + "\r\nSession: " + session_id_ +
        ";timeout=" + std::to_string(timeout_sec_) + "\r\n\r\n";
  }

  std::string handle_play(const ParsedRtspRequest &req) {
    if (session_id_.empty() || req.session.empty() || req.session != session_id_) {
      return format_error(454, "Session Not Found", req.cseq);
    }
    if (is_record_ || (state_ != RtspState::Ready && state_ != RtspState::Playing)) {
      return format_error(455, "Method Not Valid in This State", req.cseq);
    }

    state_ = RtspState::Playing;

    std::string rtp_info_hdr;
    if (!play_rtp_info_.empty()) {
      rtp_info_hdr = "RTP-Info: ";
      for (size_t i = 0; i < play_rtp_info_.size(); ++i) {
        if (i > 0U) rtp_info_hdr += ',';
        rtp_info_hdr += "url=" + play_rtp_info_[i].track_url;
        if (play_rtp_info_[i].has_seq_and_ts) {
          rtp_info_hdr += ";seq=" + std::to_string(play_rtp_info_[i].seq) +
              ";rtptime=" + std::to_string(play_rtp_info_[i].rtptime);
        }
      }
      rtp_info_hdr += "\r\n";
    } else if (stream_descriptor_.has_value()) {
      std::string base_url(req.uri);
      if (!base_url.ends_with('/')) base_url.push_back('/');
      bool first = true;
      for (const auto &tb : track_bindings_) {
        if (!tb.is_setup) continue;
        if (first) {
          rtp_info_hdr = "RTP-Info: ";
          first        = false;
        } else {
          rtp_info_hdr += ',';
        }
        rtp_info_hdr += "url=" + base_url + "trackID=" + std::to_string(tb.track_index);
      }
      if (!first) rtp_info_hdr += "\r\n";
    }

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        std::string(req.cseq) + "\r\nSession: " + session_id_ + "\r\nRange: npt=0.000-\r\n" +
        rtp_info_hdr + "\r\n";
  }

  std::string handle_record(const ParsedRtspRequest &req) {
    std::string auth_err;
    if (!check_publish_auth(req, auth_err)) {
      return auth_err;
    }
    if (session_id_.empty() || req.session.empty() || req.session != session_id_) {
      return format_error(454, "Session Not Found", req.cseq);
    }
    if (!is_record_ || (state_ != RtspState::Ready && state_ != RtspState::Recording)) {
      return format_error(455, "Method Not Valid in This State", req.cseq);
    }

    state_ = RtspState::Recording;

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        std::string(req.cseq) + "\r\nSession: " + session_id_ + "\r\nRange: npt=0.000-\r\n\r\n";
  }

  std::string handle_teardown(const ParsedRtspRequest &req) {
    if (session_id_.empty() || req.session.empty() || req.session != session_id_) {
      return format_error(454, "Session Not Found", req.cseq);
    }

    const std::string ended_session = session_id_;
    state_                          = RtspState::Init;
    session_id_.clear();
    is_record_     = false;
    has_announced_ = false;
    track_bindings_.fill(TrackTransportBinding{});
    channel_map_.fill(InterleavedChannelBinding{});
    play_rtp_info_.clear();

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        std::string(req.cseq) + "\r\nSession: " + ended_session + "\r\n\r\n";
  }
};

} //< namespace rivet::proto::rtsp
