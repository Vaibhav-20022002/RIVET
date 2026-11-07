#include "rivet/core/EgressReactor.hpp"
#include "rivet/core/IngestReactor.hpp"
#include "rivet/core/PacketRouter.hpp"
#include "rivet/core/RivetWorker.hpp"
#include "rivet/core/StreamRegistry.hpp"
#include "rivet/core/SubscriberSession.hpp"
#include "rivet/memory/NumaArena.hpp"
#include "rivet/net/SocketUtils.hpp"
#include "tests/support/ArenaLeakGuard.hpp"
#include "tests/support/H264Synth.hpp"
#include "tests/support/RtpBuilder.hpp"
#include <atomic>
#include <cstring>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace rivet::core;
using namespace rivet::memory;
using namespace rivet::test;

/// ============================================================================
/// SpscQueue Tests
/// ============================================================================

TEST(SpscQueueTest, PushPopFifoOrder) {
  SpscQueue<int, 8> queue;
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.capacity(), 8u);

  EXPECT_TRUE(queue.try_push(10));
  EXPECT_TRUE(queue.try_push(20));
  EXPECT_TRUE(queue.try_push(30));
  EXPECT_EQ(queue.size(), 3u);
  EXPECT_FALSE(queue.empty());

  int val = 0;
  EXPECT_TRUE(queue.try_pop(val));
  EXPECT_EQ(val, 10);

  EXPECT_TRUE(queue.try_pop(val));
  EXPECT_EQ(val, 20);

  EXPECT_TRUE(queue.try_pop(val));
  EXPECT_EQ(val, 30);

  EXPECT_FALSE(queue.try_pop(val));
  EXPECT_TRUE(queue.empty());
}

TEST(SpscQueueTest, CapacitySaturationRejectsPush) {
  SpscQueue<int, 4> queue;
  EXPECT_TRUE(queue.try_push(1));
  EXPECT_TRUE(queue.try_push(2));
  EXPECT_TRUE(queue.try_push(3));
  EXPECT_TRUE(queue.try_push(4));

  /// Queue is full (capacity = 4)
  EXPECT_FALSE(queue.try_push(5));
}

TEST(SpscQueueTest, TwoThreadStressMillionPacketRefsZeroLeak) {
  constexpr size_t           kTotalItems = 100'000;
  NumaArena                  arena(2048, 256);
  ArenaLeakGuard             leak_guard(arena);
  SpscQueue<PacketRef, 1024> queue;
  std::atomic<uint64_t>      consumed_sum{0};
  std::atomic<bool>          producer_done{false};

  std::thread consumer([&] {
    uint64_t  expected_seq = 1;
    PacketRef pkt;
    while (expected_seq <= kTotalItems) {
      if (queue.try_pop(pkt)) {
        EXPECT_EQ(pkt.sequence, static_cast<uint16_t>(expected_seq & 0xFFFFU));
        consumed_sum.fetch_add(1, std::memory_order_relaxed);
        ++expected_seq;
      } else if (producer_done.load(std::memory_order_acquire) && queue.empty()) {
        break;
      } else {
        std::this_thread::yield();
      }
    }
  });

  for (uint64_t seq = 1; seq <= kTotalItems; ++seq) {
    PooledBuffer *slab = nullptr;
    while ((slab = arena.allocate()) == nullptr) {
      std::this_thread::yield();
    }
    std::memset(slab->payload_data, 0x5A, 64);
    auto pkt =
        PacketRef::adopt(slab, 0, 64, 0, 90000, static_cast<uint16_t>(seq & 0xFFFFU), 0, pkt::kRef);
    while (!queue.try_push(std::move(pkt))) {
      std::this_thread::yield();
    }
  }
  producer_done.store(true, std::memory_order_release);
  consumer.join();

  EXPECT_EQ(consumed_sum.load(), kTotalItems);
  EXPECT_EQ(arena.in_flight(), 0u);
}

/// ============================================================================
/// PacketRouter Tests
/// ============================================================================

TEST(PacketRouterTest, RegisterAndBroadcast) {
  NumaArena      arena(16, 256);
  ArenaLeakGuard leak_guard(arena);
  PacketRouter   router;

  size_t chan0 = router.register_egress_channel();
  size_t chan1 = router.register_egress_channel();
  EXPECT_EQ(router.channel_count(), 2u);

  auto *q0 = router.get_channel(chan0);
  auto *q1 = router.get_channel(chan1);
  ASSERT_NE(q0, nullptr);
  ASSERT_NE(q1, nullptr);

  auto *slab1 = arena.allocate();
  auto *slab2 = arena.allocate();
  std::memset(slab1->payload_data, 0x11, 100);
  std::memset(slab2->payload_data, 0x22, 100);

  std::vector<PacketRef> batch;
  batch.push_back(PacketRef::adopt(slab1, 0, 100, 0, 90000, 1, 0, 0));
  batch.push_back(PacketRef::adopt(slab2, 0, 100, 0, 90000, 2, 0, 0));

  size_t delivered = router.broadcast(batch);
  EXPECT_EQ(delivered, 4u); //< 2 packets * 2 channels

  EXPECT_EQ(q0->size(), 2u);
  EXPECT_EQ(q1->size(), 2u);

  PacketRef out;
  EXPECT_TRUE(q0->try_pop(out));
  EXPECT_EQ(out.sequence, 1u);
  EXPECT_TRUE(q0->try_pop(out));
  EXPECT_EQ(out.sequence, 2u);
  EXPECT_TRUE(q1->try_pop(out));
  EXPECT_TRUE(q1->try_pop(out));
}

TEST(PacketRouterTest, FullSpscQueueReleasesPacketRefWithoutLeak) {
  /// When a shard's SPSC queue overflows, dropped PacketRefs must cleanly release
  /// their intrusive slab refcount so zero slabs leak!
  NumaArena      arena(16, 256);
  ArenaLeakGuard leak_guard(arena);
  PacketRouter   router;
  const size_t   ch = router.register_egress_channel();
  auto          *q  = router.get_channel(ch);
  ASSERT_NE(q, nullptr);

  auto *fill_slab = arena.allocate();
  std::memset(fill_slab->payload_data, 0xAB, 32);
  auto fill_pkt = PacketRef::adopt(fill_slab, 0, 32, 0, 90000, 1, 0, 0);

  while (q->try_push(fill_pkt)) {
  }

  /// Queue is now 100% full; broadcast one additional packet and verify drop accounting + zero leak
  auto *drop_slab = arena.allocate();
  std::memset(drop_slab->payload_data, 0xCD, 32);
  {
    std::vector<PacketRef> batch;
    batch.push_back(PacketRef::adopt(drop_slab, 0, 32, 0, 90000, 999, 0, 0));
    EXPECT_EQ(router.broadcast(batch), 0u);
    EXPECT_EQ(router.get_dropped_on_route(), 1u);
  }
  /// `drop_slab` must already be returned to the arena even before `q` drains!
  EXPECT_EQ(arena.in_flight(), 1u);

  fill_pkt.reset();
  PacketRef tmp;
  while (q->try_pop(tmp)) {
  }
  tmp.reset();
  EXPECT_EQ(arena.in_flight(), 0u);
}

/// ============================================================================
/// SubscriberSession & SubscriberTable Tests
/// ============================================================================

TEST(SubscriberSessionTest, TrackMatchingAndEnqueuePop) {
  NumaArena         arena(8, 256);
  ArenaLeakGuard    leak_guard(arena);
  SubscriberSession session(
      1001, -1, SubscriberTransport::UdpDedicatedPorts, "127.0.0.1", 5004, 5005, 0x12345678);

  session.set_track_id(0); //< Video
  EXPECT_EQ(session.get_track_id(), 0u);
  EXPECT_EQ(session.get_session_id(), 1001u);
  EXPECT_EQ(session.get_transport(), SubscriberTransport::UdpDedicatedPorts);
  EXPECT_TRUE(session.active());

  auto *slab_v = arena.allocate();
  auto *slab_a = arena.allocate();
  std::memset(slab_v->payload_data, 0x44, 100);
  std::memset(slab_a->payload_data, 0x55, 50);

  auto pkt_v = PacketRef::adopt(slab_v, 0, 100, 0, 90000, 1, 0 /* track 0 */, 0x01);
  auto pkt_a = PacketRef::adopt(slab_a, 0, 50, 0, 48000, 1, 1 /* track 1 */, 0x04);

  /// Audio packet should be rejected because subscriber track is 0 (and NOT count as a drop!)
  EXPECT_FALSE(session.enqueue_packet(pkt_a));
  EXPECT_EQ(session.queue_size(), 0u);
  EXPECT_EQ(session.get_packets_dropped(), 0u);

  /// Video packet accepted
  EXPECT_TRUE(session.enqueue_packet(pkt_v));
  EXPECT_EQ(session.queue_size(), 1u);
  EXPECT_EQ(session.get_queued_bytes(), 100u);

  /// Pop packet
  auto popped = session.pop_next_send();
  ASSERT_TRUE(popped.has_value());
  EXPECT_EQ(popped->sequence, 1u);
  EXPECT_EQ(session.queue_size(), 0u);
  EXPECT_EQ(session.get_queued_bytes(), 0u);
  EXPECT_EQ(session.get_packets_sent(), 1u);
  EXPECT_EQ(session.get_bytes_sent(), 100u);
}

TEST(SubscriberTableTest, PerTrackPartitionIsolatesVideoFromAudioDrops) {
  /// Audio packets on Track 1 never touch Video subscribers on Track 0
  /// and never inflate dropped-packet counters!
  NumaArena                 arena(16, 256);
  ArenaLeakGuard            leak_guard(arena);
  PacketRouter::PacketQueue queue;
  EgressReactor             reactor(0, &queue, 0, 0);

  auto video_sub = std::make_unique<SubscriberSession>(
      501, -1, SubscriberTransport::TcpInterleaved, "127.0.0.1", 0, 0, 0x1111);
  video_sub->set_track_id(0);
  reactor.add_subscriber(std::move(video_sub));

  /// Push 5 audio packets (track_id = 1)
  for (uint16_t s = 1; s <= 5; ++s) {
    auto *slab = arena.allocate();
    auto  bytes =
        RtpBuilder().seq(s).timestamp(48000U * s).payload({0xAA, 0xBB, 0xCC, 0xDD}).build();
    std::memcpy(slab->payload_data, bytes.data(), bytes.size());
    auto pkt = PacketRef::adopt(slab,
        0,
        static_cast<uint16_t>(bytes.size()),
        12,
        48000U * s,
        s,
        1 /* audio */,
        pkt::kRef | pkt::kMarker);
    ASSERT_TRUE(queue.try_push(std::move(pkt)));
  }

  reactor.poll_and_dispatch();
  EXPECT_EQ(reactor.get_total_dropped(), 0u);
  EXPECT_EQ(reactor.get_aggregate_queued_bytes(), 0u);
}

/// ============================================================================
/// StreamRegistry Tests
/// ============================================================================

TEST(StreamRegistryTest, RegisterRouteAndUnregister) {
  StreamRegistry registry;
  auto           worker = std::make_shared<RivetWorker>("live/camera1", 1);

  EXPECT_TRUE(registry.register_stream("live/camera1", worker));
  EXPECT_FALSE(registry.register_stream("live/camera1", worker)); //< Duplicate
  EXPECT_TRUE(registry.has_stream("live/camera1"));
  EXPECT_EQ(registry.active_stream_count(), 1u);

  /// Exact match
  EXPECT_EQ(registry.route("live/camera1"), worker);

  /// Normalization with leading slash
  EXPECT_EQ(registry.route("/live/camera1"), worker);

  /// Sub-track matching
  EXPECT_EQ(registry.route("live/camera1/streamid=0"), worker);
  EXPECT_EQ(registry.route("live/camera1/trackID=1"), worker);

  /// Full RTSP URLs
  EXPECT_EQ(registry.route("rtsps://localhost:8554/live/camera1"), worker);
  EXPECT_EQ(registry.route("rtsp://127.0.0.1:8554/live/camera1/"), worker);
  EXPECT_EQ(registry.route("rtsp://10.0.0.5:554/live/camera1/streamid=0"), worker);

  /// Unknown streams MUST return nullptr even when active_stream_count() == 1!
  EXPECT_EQ(registry.route("live/unknown"), nullptr);
  EXPECT_EQ(registry.route("live/camera2"), nullptr);
  EXPECT_EQ(registry.route("rtsp://127.0.0.1:8554/other/path"), nullptr);
  EXPECT_EQ(registry.route("live/camera10"), nullptr); //< Prefix collision guard!

  /// Unregister
  EXPECT_TRUE(registry.unregister_stream("live/camera1"));
  EXPECT_FALSE(registry.has_stream("live/camera1"));
  EXPECT_EQ(registry.active_stream_count(), 0u);
  EXPECT_EQ(registry.route("live/camera1"), nullptr);
}

/// ============================================================================
/// EgressReactor Tests
/// ============================================================================

TEST(EgressReactorTest, AddRemoveSubscriberAndStep) {
  NumaArena                 arena(8, 256);
  ArenaLeakGuard            leak_guard(arena);
  PacketRouter::PacketQueue queue;
  EgressReactor             reactor(0, &queue, 0, 0);
  ASSERT_TRUE(reactor.bind_dedicated_udp_ports());

  EXPECT_EQ(reactor.get_reactor_id(), 0u);
  EXPECT_EQ(reactor.subscriber_count(), 0u);

  /// Bind a real local ephemeral UDP socket to assert the exact forwarded RTP datagram arrives!
  int      rx_rtp_fd   = -1;
  int      rx_rtcp_fd  = -1;
  uint16_t rx_rtp_port = 0;
  uint16_t rx_rtcp_prt = 0;
  ASSERT_TRUE(rivet::net::bind_udp_port_pair(0, rx_rtp_fd, rx_rtcp_fd, rx_rtp_port, rx_rtcp_prt));

  auto sub = std::make_unique<SubscriberSession>(101,
      -1,
      SubscriberTransport::UdpDedicatedPorts,
      "127.0.0.1",
      rx_rtp_port,
      rx_rtcp_prt,
      0x12345678);
  sub->set_track_id(0);

  reactor.add_subscriber(std::move(sub));
  EXPECT_EQ(reactor.subscriber_count(), 1u);

  const auto idr_nal = H264Synth::make_idr(48);
  const auto wire =
      RtpBuilder().seq(1).timestamp(90000).ssrc(0xCAFEBABE).marker(true).payload(idr_nal).build();
  auto *slab = arena.allocate();
  ASSERT_NE(slab, nullptr);
  std::memcpy(slab->payload_data, wire.data(), wire.size());
  auto pkt = PacketRef::adopt(slab,
      0,
      static_cast<uint16_t>(wire.size()),
      12,
      90000,
      1,
      pkt::pack_track_meta(0, 0, true, false),
      pkt::kClosedIrap | pkt::kRef | pkt::kMarker);
  ASSERT_TRUE(queue.try_push(std::move(pkt)));

  /// Step reactor: polls queue, dispatches, and sends UDP datagram over real loopback socket
  reactor.step();
  EXPECT_EQ(reactor.get_total_dispatched(), 1u);

  struct pollfd pfd{};
  pfd.fd     = rx_rtp_fd;
  pfd.events = POLLIN;
  ASSERT_GT(::poll(&pfd, 1, 1000), 0);

  uint8_t       rx_buf[512]{};
  const ssize_t got = ::recv(rx_rtp_fd, rx_buf, sizeof(rx_buf), 0);
  ASSERT_EQ(got, static_cast<ssize_t>(wire.size()));
  EXPECT_EQ(std::memcmp(rx_buf, wire.data(), wire.size()), 0);

  reactor.remove_subscriber(101);
  EXPECT_EQ(reactor.subscriber_count(), 0u);
  ::close(rx_rtp_fd);
  ::close(rx_rtcp_fd);
}

TEST(EgressReactorTest, RemovingSubscriberReclaimsAggregateQueuedBytes) {
  NumaArena                 arena(8, 256);
  ArenaLeakGuard            leak_guard(arena);
  PacketRouter::PacketQueue queue;
  EgressReactor             reactor(0, &queue, 0, 0);

  auto sub = std::make_unique<SubscriberSession>(
      201, -1, SubscriberTransport::TcpInterleaved, "127.0.0.1", 0, 0, 0x1234);
  sub->set_track_id(0);
  reactor.add_subscriber(std::move(sub));

  const auto idr_nal = H264Synth::make_idr(52);
  const auto wire    = RtpBuilder().seq(1).timestamp(90000).marker(true).payload(idr_nal).build();
  auto      *slab    = arena.allocate();
  std::memcpy(slab->payload_data, wire.data(), wire.size());
  auto pkt = PacketRef::adopt(slab,
      0,
      static_cast<uint16_t>(wire.size()),
      12,
      90000,
      1,
      pkt::pack_track_meta(0, 0, true, false),
      pkt::kClosedIrap | pkt::kRef | pkt::kMarker);
  ASSERT_TRUE(queue.try_push(std::move(pkt)));

  reactor.poll_and_dispatch();
  EXPECT_EQ(reactor.get_aggregate_queued_bytes(), wire.size());

  reactor.remove_subscriber(201);
  EXPECT_EQ(reactor.get_aggregate_queued_bytes(), 0u);
}

/// ============================================================================
/// IngestReactor Tests
/// ============================================================================

TEST(IngestReactorTest, ProcessUpstreamPacketAndPublish) {
  NumaArena      arena(8, 256);
  ArenaLeakGuard leak_guard(arena);
  IngestReactor  ingest;

  /// Packet 1 enters RFC 3550 2-packet probation hold (`MIN_SEQUENTIAL = 2`)
  const auto sps_wire =
      RtpBuilder().seq(1).timestamp(90000).marker(false).payload(H264Synth::make_sps(20)).build();
  auto *slab1 = arena.allocate();
  std::memcpy(slab1->payload_data, sps_wire.data(), sps_wire.size());
  auto pkt1 = PacketRef::adopt(slab1, 0, static_cast<uint16_t>(sps_wire.size()), 0, 90000, 1, 0, 0);
  ingest.on_upstream_packet(pkt1);
  EXPECT_TRUE(ingest.publish_batch().empty());

  /// Packet 2 (`seq = 2`) confirms the source and releases both Packet 1 and Packet 2 in order!
  const auto idr_wire =
      RtpBuilder().seq(2).timestamp(90000).marker(true).payload(H264Synth::make_idr(64)).build();
  auto *slab2 = arena.allocate();
  std::memcpy(slab2->payload_data, idr_wire.data(), idr_wire.size());
  auto pkt2 =
      PacketRef::adopt(slab2, 0, static_cast<uint16_t>(idr_wire.size()), 0, 90000, 2, 0, 0x08);
  ingest.on_upstream_packet(pkt2);

  auto batch = ingest.publish_batch();
  ASSERT_EQ(batch.size(), 2u);
  EXPECT_EQ(batch[0].sequence, 1u);
  EXPECT_EQ(batch[1].sequence, 2u);

  /// Next publish batch should be empty
  auto empty_batch = ingest.publish_batch();
  EXPECT_TRUE(empty_batch.empty());
}

/// ============================================================================
/// RivetWorker Tests
/// ============================================================================

TEST(RivetWorkerTest, WorkerLifecycleAndSubscriberAssignment) {
  NumaArena   arena(8, 256);
  RivetWorker worker("live/test", 2);

  EXPECT_EQ(worker.get_stream_id(), "live/test");
  EXPECT_EQ(worker.get_egress_shard_count(), 2u);

  auto sub = std::make_unique<SubscriberSession>(
      1, -1, SubscriberTransport::UdpDedicatedPorts, "127.0.0.1", 5004, 5005, 0x1234);
  uint32_t assigned_shard = worker.assign_subscriber(std::move(sub));
  EXPECT_EQ(assigned_shard, 0u);
  EXPECT_EQ(worker.total_subscribers(), 1u);

  worker.run();
  EXPECT_TRUE(worker.is_active());

  worker.stop();
  EXPECT_FALSE(worker.is_active());
}
