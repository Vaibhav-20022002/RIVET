#pragma once

#include <atomic>
#include <cstdint>

namespace rivet::core {

/// Plain-old-data snapshot of worker & reactor telemetry counters (serializable over UDS IPC).
struct MetricsSnapshot {
  /// Ingest counters
  uint64_t ingest_packets{0};
  uint64_t ingest_bytes{0};
  uint64_t ingest_rtcp_packets{0};
  uint64_t sequence_gaps{0};
  uint64_t sequence_duplicates_or_probation{0};
  uint64_t arena_exhaustion_drops{0};
  uint64_t oversize_packet_drops{0};
  uint64_t unauthorized_packet_drops{0};
  uint64_t malformed_rtp_packets{0};

  /// HEVC / RFC 7798 malformed-input counters
  uint64_t hevc_malformed_nal_header{0}; //< F == 1 or TID == 0
  uint64_t hevc_malformed_ap{0};         //< < 2 units, nested >=48, or truncated
  uint64_t hevc_malformed_fu{0};         //< S&&E, FuType >= 48, or empty payload
  uint64_t hevc_malformed_paci{0};       //< PHSsize overflow or cType == 50
  uint64_t hevc_nonbase_layer_pkts{0};   //< LayerId > 0 forwarded opaquely
  uint64_t param_set_fu_overflow_or_gap{0};

  /// Egress counters
  uint64_t egress_packets_sent{0};
  uint64_t egress_bytes_sent{0};
  uint64_t egress_rtcp_packets{0};
  uint64_t congestion_au_drops{0};
  uint64_t congestion_packet_drops{0};
  uint64_t slow_subscriber_disconnects{0};
  uint64_t send_errors{0};
  uint64_t active_subscribers{0};
  uint64_t config_generations{0};
  uint64_t worker_restarts{0};
};

/// Cache-line-friendly atomic counter bank updated with relaxed atomics on hot paths.
struct AtomicMetrics {
  std::atomic<uint64_t> ingest_packets{0};
  std::atomic<uint64_t> ingest_bytes{0};
  std::atomic<uint64_t> ingest_rtcp_packets{0};
  std::atomic<uint64_t> sequence_gaps{0};
  std::atomic<uint64_t> sequence_duplicates_or_probation{0};
  std::atomic<uint64_t> arena_exhaustion_drops{0};
  std::atomic<uint64_t> oversize_packet_drops{0};
  std::atomic<uint64_t> unauthorized_packet_drops{0};
  std::atomic<uint64_t> malformed_rtp_packets{0};

  std::atomic<uint64_t> hevc_malformed_nal_header{0};
  std::atomic<uint64_t> hevc_malformed_ap{0};
  std::atomic<uint64_t> hevc_malformed_fu{0};
  std::atomic<uint64_t> hevc_malformed_paci{0};
  std::atomic<uint64_t> hevc_nonbase_layer_pkts{0};
  std::atomic<uint64_t> param_set_fu_overflow_or_gap{0};

  std::atomic<uint64_t> egress_packets_sent{0};
  std::atomic<uint64_t> egress_bytes_sent{0};
  std::atomic<uint64_t> egress_rtcp_packets{0};
  std::atomic<uint64_t> congestion_au_drops{0};
  std::atomic<uint64_t> congestion_packet_drops{0};
  std::atomic<uint64_t> slow_subscriber_disconnects{0};
  std::atomic<uint64_t> send_errors{0};

  [[nodiscard]] MetricsSnapshot
  snapshot(uint64_t active_subs = 0, uint64_t cfg_gen = 0, uint64_t restarts = 0) const noexcept {
    constexpr auto  R = std::memory_order_relaxed;
    MetricsSnapshot s{};
    s.ingest_packets                   = ingest_packets.load(R);
    s.ingest_bytes                     = ingest_bytes.load(R);
    s.ingest_rtcp_packets              = ingest_rtcp_packets.load(R);
    s.sequence_gaps                    = sequence_gaps.load(R);
    s.sequence_duplicates_or_probation = sequence_duplicates_or_probation.load(R);
    s.arena_exhaustion_drops           = arena_exhaustion_drops.load(R);
    s.oversize_packet_drops            = oversize_packet_drops.load(R);
    s.unauthorized_packet_drops        = unauthorized_packet_drops.load(R);
    s.malformed_rtp_packets            = malformed_rtp_packets.load(R);
    s.hevc_malformed_nal_header        = hevc_malformed_nal_header.load(R);
    s.hevc_malformed_ap                = hevc_malformed_ap.load(R);
    s.hevc_malformed_fu                = hevc_malformed_fu.load(R);
    s.hevc_malformed_paci              = hevc_malformed_paci.load(R);
    s.hevc_nonbase_layer_pkts          = hevc_nonbase_layer_pkts.load(R);
    s.param_set_fu_overflow_or_gap     = param_set_fu_overflow_or_gap.load(R);
    s.egress_packets_sent              = egress_packets_sent.load(R);
    s.egress_bytes_sent                = egress_bytes_sent.load(R);
    s.egress_rtcp_packets              = egress_rtcp_packets.load(R);
    s.congestion_au_drops              = congestion_au_drops.load(R);
    s.congestion_packet_drops          = congestion_packet_drops.load(R);
    s.slow_subscriber_disconnects      = slow_subscriber_disconnects.load(R);
    s.send_errors                      = send_errors.load(R);
    s.active_subscribers               = active_subs;
    s.config_generations               = cfg_gen;
    s.worker_restarts                  = restarts;
    return s;
  }
};

} //< namespace rivet::core
