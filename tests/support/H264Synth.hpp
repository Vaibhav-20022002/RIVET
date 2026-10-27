#pragma once

#include <cstdint>
#include <vector>

namespace rivet::test {

/// H.264 synthetic NAL unit and payload generator
class H264Synth {
public:
  /// Single NAL unit: [forbidden_zero(1) | nal_ref_idc(2) | nal_unit_type(5)] + payload
  static std::vector<uint8_t> make_nal(uint8_t nal_type, uint8_t nri, size_t payload_size = 16) {
    std::vector<uint8_t> data;
    data.reserve(1 + payload_size);
    uint8_t header = (0 << 7) | ((nri & 0x03) << 5) | (nal_type & 0x1F);
    data.push_back(header);
    for (size_t i = 0; i < payload_size; ++i) {
      data.push_back(static_cast<uint8_t>(i & 0xFF));
    }
    return data;
  }

  /// SPS: NAL type 7, NRI 3
  static std::vector<uint8_t> make_sps(size_t len = 24) { return make_nal(7, 3, len); }

  /// PPS: NAL type 8, NRI 3
  static std::vector<uint8_t> make_pps(size_t len = 8) { return make_nal(8, 3, len); }

  /// IDR Keyframe: NAL type 5, NRI 3
  static std::vector<uint8_t> make_idr(size_t len = 64) { return make_nal(5, 3, len); }

  /// Non-IDR Reference (P-frame): NAL type 1, NRI 2
  static std::vector<uint8_t> make_p_frame(size_t len = 32) { return make_nal(1, 2, len); }

  /// Non-IDR Disposable (B-frame): NAL type 1, NRI 0
  static std::vector<uint8_t> make_b_frame(size_t len = 16) { return make_nal(1, 0, len); }

  /// FU-A fragmentation of a NAL unit into chunks of max_fragment_size
  /// Each fragment has FU Indicator + FU Header + payload slice
  struct FuFragment {
    std::vector<uint8_t> payload;
    bool                 is_start{false};
    bool                 is_end{false};
  };

  static std::vector<FuFragment> fragment_fua(const std::vector<uint8_t> &nalu,
      size_t                                                              max_payload = 1400) {
    std::vector<FuFragment> frags;
    if (nalu.empty()) return frags;

    uint8_t orig_header = nalu[0];
    uint8_t nri         = (orig_header >> 5) & 0x03;
    uint8_t nal_type    = orig_header & 0x1F;

    const uint8_t *body     = nalu.data() + 1;
    size_t         body_len = nalu.size() - 1;

    size_t offset = 0;
    while (offset < body_len) {
      size_t chunk    = std::min(max_payload, body_len - offset);
      bool   is_start = (offset == 0);
      bool   is_end   = (offset + chunk == body_len);

      uint8_t fu_indicator = (0 << 7) | (nri << 5) | 28; //< NAL type 28 = FU-A
      uint8_t fu_header    = (is_start ? 0x80 : 0) | (is_end ? 0x40 : 0) | (nal_type & 0x1F);

      FuFragment frag;
      frag.is_start = is_start;
      frag.is_end   = is_end;
      frag.payload.reserve(2 + chunk);
      frag.payload.push_back(fu_indicator);
      frag.payload.push_back(fu_header);
      frag.payload.insert(frag.payload.end(), body + offset, body + offset + chunk);

      frags.push_back(std::move(frag));
      offset += chunk;
    }
    return frags;
  }
};

} //< namespace rivet::test
