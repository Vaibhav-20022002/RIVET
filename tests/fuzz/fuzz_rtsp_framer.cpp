#include "rivet/proto/rtsp/RtspStreamFramer.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rivet::fuzz {

void fuzz_rtsp_framer_one(const uint8_t *data, size_t size) {
  using namespace rivet::proto::rtsp;
  RtspReadBuffer buf(16384);

  /// Feed fuzzed bytes in variable chunk sizes (1..257 bytes) to exercise partial `$` headers,
  /// split `\r\n\r\n`, negative/overflowing `Content-Length`, and ring compaction!
  size_t off  = 0;
  size_t step = 0;
  while (off < size) {
    const size_t chunk =
        std::min(size - off, static_cast<size_t>(1 + ((step * 37 + data[off]) % 257)));
    auto span = buf.writable_span(chunk);
    if (span.size() < chunk) {
      break;
    }
    std::memcpy(span.data(), data + off, chunk);
    buf.commit_write(chunk);
    off += chunk;
    ++step;

    while (!buf.empty()) {
      const auto view = buf.next_view();
      if (view.type == FramedMessageType::None) {
        break;
      }
      if (view.type == FramedMessageType::Error) {
        buf.clear();
        return;
      }
      buf.consume(view.wire_bytes);
    }
  }
}

} //< namespace rivet::fuzz

#if defined(RIVET_LIBFUZZER_MAIN)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  rivet::fuzz::fuzz_rtsp_framer_one(data, size);
  return 0;
}
#endif
