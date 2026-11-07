#include "rivet/media/HevcPayload.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace rivet::fuzz {

void fuzz_sps_ptl_one(const uint8_t *data, size_t size) {
  using namespace rivet::media::hevc;
  const std::span<const uint8_t> sp(data, size);
  (void)parse_sps_ptl(sp);
  (void)parse_vps_info(sp);
  (void)parse_vps_id(sp);
  (void)parse_pps_id(sp);
}

} //< namespace rivet::fuzz

#if defined(RIVET_LIBFUZZER_MAIN)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  rivet::fuzz::fuzz_sps_ptl_one(data, size);
  return 0;
}
#endif
