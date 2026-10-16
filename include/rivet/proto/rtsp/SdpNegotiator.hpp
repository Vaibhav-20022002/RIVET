#pragma once

#include <string>
#include <string_view>

namespace rivet::proto::rtsp {

/// The Session Description Protocol (SDP) is used to negotiate media formats.
/// When RIVET connects to a camera, the camera sends an SDP file describing ts streams (e.g.,
/// H.264, H.265, Audio).
class SdpNegotiator {
public:
  SdpNegotiator() = default;

  /// Parses the raw SDP string received from the camera
  ///\param sdp The raw text SDP from the RTSP DESCRIBE response
  ///\return True if the SDP contains a supported video formats
  bool parse_camera_sdp(std::string_view sdp) {
    /// In a full implementation, this parses the "m=video" and "a=rtpmap" lines to extract the
    /// exact payload type mapping and clock rate.

    /// Minimal mock:
    if (sdp.find("H264") != std::string_view::npos || sdp.find("H265") != std::string_view::npos ||
        sdp.find("HEVC") != std::string_view::npos) {
      return true;
    }

    return false;
  }

  /// Synthesis an SDP string to send to a client
  /// When a client connects to RIVET, they send an SDP RTSP DESCRIBE request.
  /// RIVET must generate an SDP telling the client what format the video is in,
  /// feeding them the Configuration Generations.
  ///\param is_hevc True if the stream is H.265, false for H.264
  ///\param sps_b64 The current active Sequence Parameter Set (Base64 encoded)
  ///\param pps_b64 The current active Picture Parameter Set (Base64 encoded)
  ///\param vps_b64 The current active Video Parameter Set (Base64 encoded)
  ///\return A fully formatted SDP text string
  [[nodiscard]] std::string synthesize_client_sdp(bool is_hevc,
      const std::string                               &sps_b64,
      const std::string                               &pps_b64,
      const std::string                               &vps_b64 = "") const {
    std::string sdp = "v=0\r\n";
    sdp += "o=- 0 0 IN IP4 0.0.0.0\r\n";
    sdp += "s=RIVET Stream\r\n";
    sdp += "t=0 0\r\n";

    if (is_hevc) {
      sdp += "m=video 0 RTP/AVP 96\r\n";
      sdp += "a=rtpmap:96 H265/90000\r\n";
      sdp += "a=fmtp:96 sprop-vps=" + vps_b64 + "; sprop-sps=" + sps_b64 +
          "; sprop-pps=" + pps_b64 + ";\r\n";
    } else {
      sdp += "m=video 0 RTP/AVP 96\r\n";
      sdp += "a=rtpmap:96 H264/90000\r\n";
      /// 42e01f = Baseline Profile Level 3.1
      sdp += "a=fmtp:96 packetization-mode=1; profile-level-id=42e01f; sprop-parameter-sets=" +
          sps_b64 + "," + pps_b64 + ";\r\n";
    }

    sdp += "a=control:*\r\n";
    return sdp;
  }
};

} //< namespace rivet::proto::rtsp