#pragma once

#include "SdpNegotiator.hpp"
#include <cstdint>
#include <string>
#include <string_view>

namespace rivet::proto::rtsp {

/// The state of the RTSP connection
enum class RtspState {
  Init,     //< Just Connected
  Ready,    //< SETUP Completed
  Playing,  //< PLAY Completed, actively streaming
  Recording //< RECORD Completed (push ingest mode)
};

/// Manages the RTSP control plane for a single subscriber or camera session.
/// Handles OPTIONS, ANNOUNCE, DESCRIBE, SETUP, PLAY, RECORD, and TEARDOWN (RFC 2326).
class RtspControlSession {
  SdpNegotiator sdp_negotiator;
  RtspState     state{RtspState::Init};
  std::string   session_id{"RIVET_SESSION_1001"};
  std::string   announced_sdp;

  bool     is_record{false};
  bool     is_interleaved{false};
  uint16_t client_rtp_port{5004};
  uint16_t client_rtcp_port{5005};
  uint16_t base_rtp_port{20000};
  uint16_t base_rtcp_port{20001};
  uint16_t server_rtp_port{20000};
  uint16_t server_rtcp_port{20001};
  uint8_t  interleaved_channel{0};

public:
  explicit RtspControlSession(uint16_t rtp_port = 20000, uint16_t rtcp_port = 20001)
      : base_rtp_port(rtp_port)
      , base_rtcp_port(rtcp_port)
      , server_rtp_port(rtp_port)
      , server_rtcp_port(rtcp_port) {}

  void set_base_ports(uint16_t rtp, uint16_t rtcp) noexcept {
    base_rtp_port    = rtp;
    base_rtcp_port   = rtcp;
    server_rtp_port  = rtp;
    server_rtcp_port = rtcp;
  }

  /// Handles incoming RTSP requests and generates the appropriate RFC 2326 text response
  std::string handle_request(std::string_view request) {
    std::string cseq = extract_cseq(request);

    if (request.starts_with("OPTIONS")) {
      return handle_options(request, cseq);
    } else if (request.starts_with("ANNOUNCE")) {
      return handle_announce(request, cseq);
    } else if (request.starts_with("DESCRIBE")) {
      return handle_describe(request, cseq);
    } else if (request.starts_with("SETUP")) {
      return handle_setup(request, cseq);
    } else if (request.starts_with("PLAY")) {
      return handle_play(request, cseq);
    } else if (request.starts_with("RECORD")) {
      return handle_record(request, cseq);
    } else if (request.starts_with("TEARDOWN")) {
      return handle_teardown(request, cseq);
    }

    return "RTSP/1.0 400 Bad Request\r\nCSeq: " + cseq + "\r\n\r\n";
  }

  [[nodiscard]] RtspState get_state() const noexcept { return state; }
  [[nodiscard]] bool      is_record_mode() const noexcept { return is_record; }
  [[nodiscard]] bool      is_interleaved_mode() const noexcept { return is_interleaved; }
  [[nodiscard]] uint16_t  get_client_rtp_port() const noexcept { return client_rtp_port; }
  [[nodiscard]] uint16_t  get_client_rtcp_port() const noexcept { return client_rtcp_port; }
  [[nodiscard]] uint8_t   get_interleaved_channel() const noexcept { return interleaved_channel; }
  [[nodiscard]] const std::string &get_announced_sdp() const noexcept { return announced_sdp; }
  void                             set_server_ports(uint16_t rtp, uint16_t rtcp) noexcept {
    server_rtp_port  = rtp;
    server_rtcp_port = rtcp;
  }
  void set_announced_sdp(std::string sdp) { announced_sdp = std::move(sdp); }

private:
  static std::string extract_cseq(std::string_view request) {
    constexpr std::string_view key = "CSeq: ";
    auto                       pos = request.find(key);
    if (pos == std::string_view::npos) {
      return "1";
    }
    pos += key.size();
    auto end_pos = request.find_first_of("\r\n", pos);
    if (end_pos == std::string_view::npos) {
      return std::string(request.substr(pos));
    }
    return std::string(request.substr(pos, end_pos - pos));
  }

  std::string handle_options(std::string_view, const std::string &cseq) const {
    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq +
        "\r\n"
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, ANNOUNCE, RECORD\r\n\r\n";
  }

  std::string handle_announce(std::string_view request, const std::string &cseq) {
    auto body_pos = request.find("\r\n\r\n");
    if (body_pos != std::string_view::npos) {
      announced_sdp = std::string(request.substr(body_pos + 4));
    }
    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq + "\r\n\r\n";
  }

  std::string handle_describe(std::string_view request, const std::string &cseq) const {
    std::string sdp = announced_sdp;
    if (sdp.empty()) {
      sdp = sdp_negotiator.synthesize_client_sdp(false, "Z0LAH9gPRA==", "aM48gA==");
    }

    std::string content_base;
    auto        sp1 = request.find(' ');
    if (sp1 != std::string_view::npos) {
      auto sp2 = request.find(' ', sp1 + 1);
      if (sp2 != std::string_view::npos) {
        std::string url = std::string(request.substr(sp1 + 1, sp2 - (sp1 + 1)));
        if (!url.ends_with('/')) {
          url += '/';
        }
        content_base = "Content-Base: " + url + "\r\n";
      }
    }

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq + "\r\n" + content_base +
        "Content-Type: application/sdp\r\n"
        "Content-Length: " +
        std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
  }

  std::string handle_setup(std::string_view request, const std::string &cseq) {
    state = RtspState::Ready;

    if (request.find("mode=record") != std::string_view::npos ||
        request.find("RECORD") != std::string_view::npos) {
      is_record = true;
    }

    if (request.find("TCP") != std::string_view::npos ||
        request.find("interleaved") != std::string_view::npos) {
      is_interleaved = true;
    }

    if (request.find("streamid=1") != std::string_view::npos ||
        request.find("trackID=2") != std::string_view::npos ||
        request.find("track2") != std::string_view::npos) {
      server_rtp_port  = static_cast<uint16_t>(base_rtp_port + 2);
      server_rtcp_port = static_cast<uint16_t>(base_rtcp_port + 2);
    } else {
      server_rtp_port  = base_rtp_port;
      server_rtcp_port = base_rtcp_port;
    }

    auto        client_port_pos = request.find("client_port=");
    std::string client_port_str = "5004-5005";
    if (client_port_pos != std::string_view::npos) {
      auto end_cp = request.find_first_of(";\r\n", client_port_pos);
      client_port_str =
          std::string(request.substr(client_port_pos + 12, end_cp - (client_port_pos + 12)));
      auto dash = client_port_str.find('-');
      if (dash != std::string::npos) {
        client_rtp_port = static_cast<uint16_t>(std::atoi(client_port_str.substr(0, dash).c_str()));
        client_rtcp_port =
            static_cast<uint16_t>(std::atoi(client_port_str.substr(dash + 1).c_str()));
      }
    }

    auto        inter_pos       = request.find("interleaved=");
    std::string interleaved_str = "0-1";
    if (inter_pos != std::string_view::npos) {
      auto end_ip     = request.find_first_of(";\r\n", inter_pos);
      interleaved_str = std::string(request.substr(inter_pos + 12, end_ip - (inter_pos + 12)));
      auto dash       = interleaved_str.find('-');
      if (dash != std::string::npos) {
        interleaved_channel =
            static_cast<uint8_t>(std::atoi(interleaved_str.substr(0, dash).c_str()));
      }
    }

    std::string transport_hdr;
    if (is_interleaved) {
      transport_hdr = "Transport: RTP/AVP/TCP;unicast;interleaved=" + interleaved_str;
    } else {
      transport_hdr = "Transport: RTP/AVP/UDP;unicast;client_port=" + client_port_str +
          ";server_port=" + std::to_string(server_rtp_port) + "-" +
          std::to_string(server_rtcp_port);
    }

    if (is_record) {
      transport_hdr += ";mode=record";
    }

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq + "\r\n" + transport_hdr + "\r\nSession: " + session_id + "\r\n\r\n";
  }

  std::string handle_play(std::string_view, const std::string &cseq) {
    state = RtspState::Playing;

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq + "\r\nSession: " + session_id + "\r\nRange: npt=0.000-\r\n\r\n";
  }

  std::string handle_record(std::string_view, const std::string &cseq) {
    state = RtspState::Recording;

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq + "\r\nSession: " + session_id + "\r\nRange: npt=0.000-\r\n\r\n";
  }

  std::string handle_teardown(std::string_view, const std::string &cseq) {
    state = RtspState::Init;

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq + "\r\nSession: " + session_id + "\r\n\r\n";
  }
};

} //< namespace rivet::proto::rtsp