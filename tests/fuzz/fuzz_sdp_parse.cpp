#include "rivet/media/ConfigGeneration.hpp"
#include "rivet/media/StreamDescriptor.hpp"
#include "rivet/proto/rtsp/SdpNegotiator.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rivet::fuzz {

void fuzz_sdp_parse_one(const uint8_t *data, size_t size) {
  const std::string_view sv(reinterpret_cast<const char *>(data), size);
  const auto             res = rivet::media::parse_announce_sdp(sv);
  if (res.error == rivet::media::SdpError::None && res.descriptor.has_value()) {
    rivet::media::ConfigGeneration gen;
    const auto sdp = rivet::proto::rtsp::SdpNegotiator::render_sdp(*res.descriptor, gen, "0.0.0.0");
    (void)sdp.size();
  }
}

} //< namespace rivet::fuzz

#if defined(RIVET_LIBFUZZER_MAIN)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  rivet::fuzz::fuzz_sdp_parse_one(data, size);
  return 0;
}
#endif
