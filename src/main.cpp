#include "rivet/core/IngestReactor.hpp"
#include "rivet/core/PacketRouter.hpp"
#include "rivet/media/AuAssembler.hpp"
#include "rivet/media/ConfigGeneration.hpp"
#include "rivet/media/DependencyDropPolicy.hpp"
#include "rivet/media/StartupPolicy.hpp"
#include "rivet/proto/rtp/RtcpReportGenerator.hpp"
#include "rivet/proto/rtp/SequenceTracker.hpp"
#include "rivet/proto/rtsp/RtspControlSession.hpp"
#include <iostream>

using namespace rivet;

int main(void) {
  std::cout << "RIVET engine started successfully!\n";
  std::cout << "Platform: ";
#ifdef __linux__
  std::cout << "Linux (io_uring zero-copy networking ENABLED)\n";
#else
  std::cout << "macOS (io_uring DISABLED - Development Mode)\n";
#endif

  core::IngestReactor ingest_reactor;
  ingest_reactor.run();
  std::cout << "Ingest reactor initialized, RTSP State: "
            << static_cast<int>(ingest_reactor.get_rtsp_state()) << "\n";

  core::PacketRouter router;
  size_t             chan0 = router.register_egress_channel();
  size_t             chan1 = router.register_egress_channel();
  std::cout << "PacketRouter initialized with " << router.channel_count()
            << " channels (IDs: " << chan0 << ", " << chan1 << ")\n";

  ingest_reactor.stop();

  return 0;
}