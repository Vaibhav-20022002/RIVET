#pragma once

#include "SdpNegotiator.hpp"
#include <string>
#include <string_view>

namespace rivet::proto::rtsp {

/// The state of the RTSP connection
enum class RtspState {
  Init,     //< Just Connected
  Ready,    //< SETUP Completed
  Playing,  //< PLAY Completed, actively streaming
  Recording //< RECORD Completed (push ingest, ARCHITECTURE.md §1904)
};

/// Manages the RTSP control plane for a single subscriber or camera.
/// RTSP runs over TCP, whereas the actual video (RTP) runs over UDP via io_uring.
class RtspControlSession {
  SdpNegotiator sdp_negotiator;
  RtspState     state{RtspState::Init};
  std::string   session_id;

public:
  RtspControlSession() = default;

  /// Handles incoming RTSP requests (OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN) and generates the
  /// appropriate RFC 2326 text response with CSeq echoed back to the client.
  std::string handle_request(std::string_view request) {
    std::string cseq = extract_cseq(request);

    if (request.starts_with("OPTIONS")) {
      return handle_options(request, cseq);
    } else if (request.starts_with("DESCRIBE")) {
      return handle_describe(request, cseq);
    } else if (request.starts_with("SETUP")) {
      return handle_setup(request, cseq);
    } else if (request.starts_with("PLAY")) {
      return handle_play(request, cseq);
    } else if (request.starts_with("TEARDOWN")) {
      return handle_teardown(request, cseq);
    }

    return "RTSP/1.0 400 Bad Request\r\n"
           "CSeq: " +
        cseq + "\r\n\r\n";
  }

  [[nodiscard]] RtspState get_state() const noexcept { return state; }

private:
  /// Extracts the CSeq header value required by RFC 2326 §12.17
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
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n";
  }

  std::string handle_describe(std::string_view, const std::string &cseq) const {
    /// Pull parameters for Baseline Profile H.264
    std::string sdp = sdp_negotiator.synthesize_client_sdp(false, "Z0LAH9gPRA==", "aM48gA==");

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq +
        "\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " +
        std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
  }

  std::string handle_setup(std::string_view, const std::string &cseq) {
    state      = RtspState::Ready;
    session_id = "RIVET_SESSION_9999";

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq +
        "\r\n"
        "Transport: RTP/AVP;unicast;client_port=9000-9001;server_port=8000-8001\r\n"
        "Session: " +
        session_id + "\r\n\r\n";
  }

  std::string handle_play(std::string_view, const std::string &cseq) {
    state = RtspState::Playing;

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq +
        "\r\n"
        "Session: " +
        session_id + "\r\n\r\n";
  }

  std::string handle_teardown(std::string_view, const std::string &cseq) {
    state = RtspState::Init;

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " +
        cseq +
        "\r\n"
        "Session: " +
        session_id + "\r\n\r\n";
  }
};

} //< namespace rivet::proto::rtsp