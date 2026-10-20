#pragma once

#include "rivet/core/EgressReactor.hpp"
#include "rivet/core/IngestReactor.hpp"
#include "rivet/core/PacketRouter.hpp"
#include "rivet/core/SubscriberSession.hpp"
#include "rivet/memory/PacketRef.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::core {

/// RivetWorker is the isolated per-stream worker process container.
/// Coordinates Thread 0 (IngestReactor) with Threads 1..N (EgressReactors) via lock-free SPSC
/// channels.
class RivetWorker {
  std::string   stream_id;
  IngestReactor ingest;
  PacketRouter  router;

  std::vector<std::unique_ptr<EgressReactor>> egress_reactors;

  std::atomic<bool> is_running{false};
  uint32_t          egress_shard_count{1};

public:
  explicit RivetWorker(std::string id,
      uint32_t                     num_egress_shards = 2,
      uint16_t                     base_rtp          = 20000,
      uint16_t                     base_rtcp         = 20001)
      : stream_id(std::move(id))
      , egress_shard_count(num_egress_shards > 0 ? num_egress_shards : 1) {
    egress_reactors.reserve(egress_shard_count);

    /// Initialize dedicated lock-free SPSC channels and EgressReactor shards
    for (uint32_t i = 0; i < egress_shard_count; ++i) {
      size_t chan_id  = router.register_egress_channel();
      auto  *chan_ptr = router.get_channel(chan_id);
      egress_reactors.push_back(std::make_unique<EgressReactor>(i, chan_ptr, base_rtp, base_rtcp));
    }
  }

  ~RivetWorker() { stop(); }

  /// Non-copyable and non-movable process container
  RivetWorker(const RivetWorker &)            = delete;
  RivetWorker &operator=(const RivetWorker &) = delete;
  RivetWorker(RivetWorker &&)                 = delete;
  RivetWorker &operator=(RivetWorker &&)      = delete;

  [[nodiscard]] std::string_view get_stream_id() const noexcept { return stream_id; }

  [[nodiscard]] const IngestReactor &get_ingest() const noexcept { return ingest; }

  [[nodiscard]] IngestReactor &get_ingest() noexcept { return ingest; }

  [[nodiscard]] const std::string &get_sdp() const noexcept {
    return ingest.get_rtsp_session().get_announced_sdp();
  }

  void set_sdp(std::string sdp) { ingest.get_rtsp_session().set_announced_sdp(std::move(sdp)); }

  [[nodiscard]] const PacketRouter &get_router() const noexcept { return router; }

  [[nodiscard]] PacketRouter &get_router() noexcept { return router; }

  [[nodiscard]] size_t get_egress_shard_count() const noexcept { return egress_reactors.size(); }

  [[nodiscard]] EgressReactor *get_egress_reactor(size_t index) noexcept {
    if (index < egress_reactors.size()) {
      return egress_reactors[index].get();
    }
    return nullptr;
  }

  void set_udp_sockets(int v_sock, int a_sock) noexcept {
    for (auto &reactor : egress_reactors) {
      reactor->set_udp_sockets(v_sock, a_sock);
    }
  }

  /// Assigns a new subscriber to the least-loaded EgressReactor shard
  ///\param session Unique pointer to SubscriberSession
  ///\return Shard ID that received the subscriber
  uint32_t assign_subscriber(std::unique_ptr<SubscriberSession> session) {
    if (egress_reactors.empty() || !session) {
      return 0;
    }

    /// Find least loaded shard
    uint32_t best_shard      = 0;
    size_t   min_subscribers = egress_reactors[0]->subscriber_count();

    for (size_t i = 1; i < egress_reactors.size(); ++i) {
      if (egress_reactors[i]->subscriber_count() < min_subscribers) {
        min_subscribers = egress_reactors[i]->subscriber_count();
        best_shard      = static_cast<uint32_t>(i);
      }
    }

    /// Add subscriber with access to RetainedPacketLog for fast catch-up burst
    egress_reactors[best_shard]->add_subscriber(std::move(session), &ingest.get_packet_log());
    return best_shard;
  }

  /// Processes an incoming raw packet on the ingest path, then fans it out to all egress shards
  ///\param packet The incoming owning PacketRef handle
  ///\return Assembled AccessUnitView if this packet completed a frame, or std::nullopt
  std::optional<media::AccessUnitView> process_incoming_packet(memory::PacketRef packet) {
    auto au = ingest.on_upstream_packet(std::move(packet));

    /// Flush published packet batch across lock-free SPSC channels to Egress Reactors
    auto batch = ingest.publish_batch();
    if (!batch.empty()) {
      router.broadcast(batch);
    }

    /// Step each egress reactor to poll the SPSC queue and dispatch to viewers
    for (auto &reactor : egress_reactors) {
      reactor->step();
    }

    return au;
  }

  /// Starts all reactor threads in the worker
  void run() {
    is_running.store(true, std::memory_order_release);
    ingest.run();
    for (auto &reactor : egress_reactors) {
      reactor->run();
    }
  }

  /// Signals all reactor threads in the worker to terminate cleanly
  void stop() noexcept {
    if (is_running.exchange(false, std::memory_order_acq_rel)) {
      ingest.stop();
      for (auto &reactor : egress_reactors) {
        reactor->stop();
      }
    }
  }

  [[nodiscard]] bool is_active() const noexcept {
    return is_running.load(std::memory_order_acquire);
  }

  /// Total count of active subscribers across all shards
  [[nodiscard]] size_t total_subscribers() const noexcept {
    size_t count = 0;
    for (const auto &reactor : egress_reactors) {
      count += reactor->subscriber_count();
    }
    return count;
  }
};

} //< namespace rivet::core
