#include "rivet/core/EgressReactor.hpp"
#include "rivet/core/IngestReactor.hpp"
#include "rivet/core/PacketRouter.hpp"
#include "rivet/core/RivetWorker.hpp"
#include "rivet/core/StreamRegistry.hpp"
#include "rivet/core/SubscriberSession.hpp"
#include "rivet/memory/NumaArena.hpp"
#include <gtest/gtest.h>

using namespace rivet::core;
using namespace rivet::memory;

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

/// ============================================================================
/// PacketRouter Tests
/// ============================================================================

TEST(PacketRouterTest, RegisterAndBroadcast) {
  NumaArena    arena(16, 256);
  PacketRouter router;

  size_t chan0 = router.register_egress_channel();
  size_t chan1 = router.register_egress_channel();
  EXPECT_EQ(router.channel_count(), 2u);

  auto *q0 = router.get_channel(chan0);
  auto *q1 = router.get_channel(chan1);
  ASSERT_NE(q0, nullptr);
  ASSERT_NE(q1, nullptr);

  auto *slab1 = arena.allocate();
  auto *slab2 = arena.allocate();

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
}

/// ============================================================================
/// SubscriberSession Tests
/// ============================================================================

TEST(SubscriberSessionTest, TrackMatchingAndEnqueuePop) {
  NumaArena         arena(8, 256);
  SubscriberSession session(
      1001, -1, SubscriberTransport::UdpDedicatedPorts, "127.0.0.1", 5004, 5005, 0x12345678);

  session.set_track_id(0); //< Video
  EXPECT_EQ(session.get_track_id(), 0u);
  EXPECT_EQ(session.get_session_id(), 1001u);
  EXPECT_EQ(session.get_transport(), SubscriberTransport::UdpDedicatedPorts);
  EXPECT_TRUE(session.active());

  auto *slab_v = arena.allocate();
  auto *slab_a = arena.allocate();

  auto pkt_v = PacketRef::adopt(slab_v, 0, 100, 0, 90000, 1, 0 /* track 0 */, 0x01);
  auto pkt_a = PacketRef::adopt(slab_a, 0, 50, 0, 48000, 1, 1 /* track 1 */, 0x04);

  /// Audio packet should be rejected because subscriber track is 0
  EXPECT_FALSE(session.enqueue_packet(pkt_a));
  EXPECT_EQ(session.queue_size(), 0u);

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

  /// Full RTSP URLs
  EXPECT_EQ(registry.route("rtsps://localhost:8554/live/camera1"), worker);
  EXPECT_EQ(registry.route("rtsp://127.0.0.1:8554/live/camera1/"), worker);
  EXPECT_EQ(registry.route("rtsp://10.0.0.5:554/live/camera1/streamid=0"), worker);
  EXPECT_EQ(registry.route("rtsp://10.0.0.5:554/live/camera1/streamid=0"), worker);

  /// Unregister
  EXPECT_TRUE(registry.unregister_stream("live/camera1"));
  EXPECT_FALSE(registry.has_stream("live/camera1"));
  EXPECT_EQ(registry.active_stream_count(), 0u);
}

/// ============================================================================
/// EgressReactor Tests
/// ============================================================================

TEST(EgressReactorTest, AddRemoveSubscriberAndStep) {
  NumaArena                 arena(8, 256);
  PacketRouter::PacketQueue queue;
  EgressReactor             reactor(0, &queue);

  EXPECT_EQ(reactor.get_reactor_id(), 0u);
  EXPECT_EQ(reactor.subscriber_count(), 0u);

  auto sub = std::make_unique<SubscriberSession>(
      101, -1, SubscriberTransport::UdpDedicatedPorts, "127.0.0.1", 5004, 5005, 0x1234);
  sub->set_track_id(0);

  reactor.add_subscriber(std::move(sub));
  EXPECT_EQ(reactor.subscriber_count(), 1u);

  auto *slab = arena.allocate();
  auto  pkt  = PacketRef::adopt(slab, 0, 64, 0, 90000, 1, 0, 0x01);
  queue.try_push(pkt);

  /// Step reactor: polls queue and dispatches
  reactor.step();
  EXPECT_EQ(reactor.get_total_dispatched(), 1u);

  reactor.remove_subscriber(101);
  EXPECT_EQ(reactor.subscriber_count(), 0u);
}

TEST(EgressReactorTest, RemovingSubscriberReclaimsAggregateQueuedBytes) {
  NumaArena                 arena(8, 256);
  PacketRouter::PacketQueue queue;
  EgressReactor             reactor(0, &queue);

  auto sub = std::make_unique<SubscriberSession>(
      201, -1, SubscriberTransport::TcpInterleaved, "127.0.0.1", 0, 0, 0x1234);
  sub->set_track_id(0);
  reactor.add_subscriber(std::move(sub));

  auto *slab = arena.allocate();
  auto  pkt  = PacketRef::adopt(slab, 0, 64, 0, 90000, 1, 0, 0x01);
  queue.try_push(pkt);

  reactor.poll_and_dispatch();
  EXPECT_EQ(reactor.get_aggregate_queued_bytes(), 64u);

  reactor.remove_subscriber(201);
  EXPECT_EQ(reactor.get_aggregate_queued_bytes(), 0u);
}

/// ============================================================================
/// IngestReactor Tests
/// ============================================================================

TEST(IngestReactorTest, ProcessUpstreamPacketAndPublish) {
  NumaArena     arena(8, 256);
  IngestReactor ingest;

  auto *slab = arena.allocate();
  auto  pkt  = PacketRef::adopt(slab, 0, 100, 0, 90000, 1, 0, 0x08);

  ingest.on_upstream_packet(pkt);
  auto batch = ingest.publish_batch();
  EXPECT_EQ(batch.size(), 1u);
  EXPECT_EQ(batch[0].sequence, 1u);

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
