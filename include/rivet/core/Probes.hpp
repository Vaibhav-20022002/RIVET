#pragma once

#include <cstdint>

#if defined(RIVET_ENABLE_PROBES)

namespace rivet::core::probes {

enum class ProbePoint : uint8_t {
  IngestPacketReceived,
  IngestAuAssembled,
  IngestBatchPublished,
  RouterBroadcast,
  EgressSpscPopped,
  EgressSubscriberQueued,
  EgressPacketFlushed
};

using ProbeCallback = void (*)(ProbePoint point,
    uint64_t                              arrival_ts,
    uint16_t                              seq,
    uint8_t                               track_id) noexcept;

inline ProbeCallback g_probe_cb{nullptr};

inline void set_probe_callback(ProbeCallback cb) noexcept {
  g_probe_cb = cb;
}

inline void record(ProbePoint point, uint64_t arrival_ts, uint16_t seq, uint8_t track_id) noexcept {
  if (g_probe_cb) {
    g_probe_cb(point, arrival_ts, seq, track_id);
  }
}

} //< namespace rivet::core::probes

#define RIVET_PROBE(point, arrival_ts, seq, track_id) \
  ::rivet::core::probes::record(::rivet::core::probes::ProbePoint::point, arrival_ts, seq, track_id)

#else

#define RIVET_PROBE(point, arrival_ts, seq, track_id) ((void)0)

#endif
