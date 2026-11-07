#include "rivet/media/HevcPayload.hpp"
#include "rivet/proto/rtp/RtpPacketClassifier.hpp"
#include <cstddef>
#include <cstdint>

namespace rivet::fuzz {

void fuzz_hevc_classify_one(const uint8_t *data, size_t size) {
  using namespace rivet::media;
  (void)hevc::classify(data, size, false);
  (void)hevc::classify(data, size, true);

  hevc::for_each_parameter_set(data, size, false, [](uint8_t, const uint8_t *, size_t) noexcept {});
  hevc::for_each_parameter_set(data, size, true, [](uint8_t, const uint8_t *, size_t) noexcept {});

  hevc::FuParamReassembler reasm;
  (void)reasm.on_fu_packet(data, size, 1, false, [](uint8_t, const uint8_t *, size_t) noexcept {});

  TrackDescriptor h264_trk{};
  h264_trk.index = 0;
  h264_trk.kind  = MediaKind::Video;
  h264_trk.codec = VideoCodec::H264;

  TrackDescriptor h265_trk{};
  h265_trk.index                   = 0;
  h265_trk.kind                    = MediaKind::Video;
  h265_trk.codec                   = VideoCodec::H265;
  h265_trk.hevc.sprop_max_don_diff = 1;

  (void)rivet::proto::rtp::RtpPacketClassifier::classify(data, size, h264_trk);
  (void)rivet::proto::rtp::RtpPacketClassifier::classify(data, size, h265_trk);
}

} //< namespace rivet::fuzz

#if defined(RIVET_LIBFUZZER_MAIN)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  rivet::fuzz::fuzz_hevc_classify_one(data, size);
  return 0;
}
#endif
