#include "tests/support/HevcSynth.hpp"
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

namespace rivet::fuzz {
void fuzz_rtsp_framer_one(const uint8_t *data, size_t size);
void fuzz_sdp_parse_one(const uint8_t *data, size_t size);
void fuzz_hevc_classify_one(const uint8_t *data, size_t size);
void fuzz_sps_ptl_one(const uint8_t *data, size_t size);
} //< namespace rivet::fuzz

namespace {

std::vector<uint8_t> mutate_seed(const std::vector<uint8_t> &seed, std::mt19937 &rng) {
  std::vector<uint8_t> out = seed;
  const uint32_t       op  = rng() % 5;
  if (op == 0 && !out.empty()) {
    /// Bit-flip 1..4 positions
    const size_t flips = 1 + (rng() % 4);
    for (size_t i = 0; i < flips; ++i) {
      out[rng() % out.size()] ^= static_cast<uint8_t>(1U << (rng() % 8));
    }
  } else if (op == 1 && !out.empty()) {
    /// Truncate
    out.resize(rng() % out.size());
  } else if (op == 2) {
    /// Splice extreme byte patterns (`0x00 00 03`, `0xFF`, `$`, `\r\n`)
    const uint8_t specials[] = {0x00, 0x00, 0x03, 0xFF, '$', '\r', '\n', '-', '9'};
    const size_t  pos        = out.empty() ? 0 : (rng() % out.size());
    out.insert(
        out.begin() + static_cast<std::ptrdiff_t>(pos), std::begin(specials), std::end(specials));
  } else if (op == 3 && !out.empty()) {
    /// Overwrite random byte with boundary value
    const uint8_t bounds[]  = {0x00, 0x01, 0x24, 0x60, 0x62, 0x64, 0x7F, 0x80, 0xFF};
    out[rng() % out.size()] = bounds[rng() % sizeof(bounds)];
  } else {
    /// Pure random buffer 0..512 bytes
    const size_t len = rng() % 513;
    out.resize(len);
    for (size_t i = 0; i < len; ++i) {
      out[i] = static_cast<uint8_t>(rng() & 0xFFU);
    }
  }
  return out;
}

} //< namespace

TEST(FuzzSmokeSuite, TenThousandAdversarialMutationsAcrossAllFourParsers) {
  using rivet::test::HevcSynth;
  std::mt19937 rng(0xF0225EEDU);

  /// Rich structured corpus seeds for each parser
  const std::string          rtsp_seed_str = "ANNOUNCE rtsp://<127.0.0.1:8554/live/cam RTSP/1.0\r\n"
                                             "CSeq: 2\r\nContent-Length: -43\r\n\r\n"
                                             "$0\x00\x10"
                                             "0123456789ABCDEF";
  const std::vector<uint8_t> rtsp_seed(rtsp_seed_str.begin(), rtsp_seed_str.end());

  const std::string sdp_seed_str =
      "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Fuzz\r\nt=0 0\r\n"
      "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H265/90000\r\n"
      "a=fmtp:96 profile-id=1;level-id=120;sprop-vps=QAEMAf//<AWAAAAMAAAMAAAMAAAMAeKwJ;"
      "sprop-max-don-diff=1;tx-mode=SRST\r\na=control:streamid=0\r\n";
  const std::vector<uint8_t> sdp_seed(sdp_seed_str.begin(), sdp_seed_str.end());

  const std::vector<uint8_t> hevc_ap_seed =
      HevcSynth::make_ap({HevcSynth::make_vps(),
                             HevcSynth::make_sps(0, 0, 2, 0, 0, 1, 120, true),
                             HevcSynth::make_idr(32)},
          1,
          true,
          42);
  const std::vector<uint8_t> sps_seed = HevcSynth::make_sps(1, 0, 3, 0, 1, 2, 153, true);

  /// Empty and 1-byte edge cases first
  const uint8_t zero = 0;
  rivet::fuzz::fuzz_rtsp_framer_one(nullptr, 0);
  rivet::fuzz::fuzz_rtsp_framer_one(&zero, 1);
  rivet::fuzz::fuzz_sdp_parse_one(nullptr, 0);
  rivet::fuzz::fuzz_sdp_parse_one(&zero, 1);
  rivet::fuzz::fuzz_hevc_classify_one(nullptr, 0);
  rivet::fuzz::fuzz_hevc_classify_one(&zero, 1);
  rivet::fuzz::fuzz_sps_ptl_one(nullptr, 0);
  rivet::fuzz::fuzz_sps_ptl_one(&zero, 1);

  /// 2,500 mutations * 4 targets = 10,000 adversarial parser invocations under ASan/UBSan/TSan!
  for (int i = 0; i < 2500; ++i) {
    const auto m_rtsp = mutate_seed(rtsp_seed, rng);
    rivet::fuzz::fuzz_rtsp_framer_one(m_rtsp.data(), m_rtsp.size());

    const auto m_sdp = mutate_seed(sdp_seed, rng);
    rivet::fuzz::fuzz_sdp_parse_one(m_sdp.data(), m_sdp.size());

    const auto m_hevc = mutate_seed(hevc_ap_seed, rng);
    rivet::fuzz::fuzz_hevc_classify_one(m_hevc.data(), m_hevc.size());

    const auto m_sps = mutate_seed(sps_seed, rng);
    rivet::fuzz::fuzz_sps_ptl_one(m_sps.data(), m_sps.size());
  }
}
