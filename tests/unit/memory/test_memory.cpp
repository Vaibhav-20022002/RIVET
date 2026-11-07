#include "rivet/memory/MemoryPool.hpp"
#include "rivet/memory/NumaArena.hpp"
#include "rivet/memory/PacketFlags.hpp"
#include "rivet/memory/PacketRef.hpp"
#include "rivet/memory/PooledBuffer.hpp"
#include "tests/support/ArenaLeakGuard.hpp"
#include <atomic>
#include <gtest/gtest.h>
#include <limits>
#include <thread>
#include <vector>

using namespace rivet::memory;

/// ============================================================================
/// PooledBuffer Tests
/// ============================================================================

TEST(PooledBufferTest, InitialStateAndRetainRelease) {
  NumaArena arena(4, 128);
  auto     *buf = arena.allocate();
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(buf->ref_count.load(), 1u);
  EXPECT_EQ(buf->size(), 128u);
  EXPECT_NE(buf->data(), nullptr);

  buf->retain();
  EXPECT_EQ(buf->ref_count.load(), 2u);

  buf->release();
  EXPECT_EQ(buf->ref_count.load(), 1u);
  EXPECT_EQ(arena.in_flight(), 1u);

  buf->release();
  EXPECT_EQ(arena.in_flight(), 0u);
}

TEST(PooledBufferTest, BatchInitRefsAndReleaseN) {
  NumaArena arena(4, 256);
  auto     *buf = arena.allocate();
  ASSERT_NE(buf, nullptr);

  buf->init_refs(5);
  EXPECT_EQ(buf->ref_count.load(), 5u);

  buf->release_n(3);
  EXPECT_EQ(buf->ref_count.load(), 2u);
  EXPECT_EQ(arena.in_flight(), 1u);

  buf->release_n(2);
  EXPECT_EQ(arena.in_flight(), 0u);
}

/// ============================================================================
/// NumaArena & MultiClassArena Tests
/// ============================================================================

TEST(NumaArenaTest, CapacityAndAlignment) {
  NumaArena arena(64, 2048);
  EXPECT_EQ(arena.total_capacity(), 64u);
  EXPECT_EQ(arena.available(), 64u);
  EXPECT_EQ(arena.in_flight(), 0u);
  EXPECT_EQ(arena.get_slab_size(), 2048u);

  auto *slab = arena.allocate();
  ASSERT_NE(slab, nullptr);
  EXPECT_EQ(arena.available(), 63u);
  EXPECT_EQ(arena.in_flight(), 1u);

  auto addr = reinterpret_cast<uintptr_t>(slab->payload_data);
  EXPECT_EQ(addr % 64, 0u);

  slab->release();
  EXPECT_EQ(arena.available(), 64u);
  EXPECT_EQ(arena.in_flight(), 0u);
}

TEST(NumaArenaTest, ExhaustionReturnsNullptrAndCounts) {
  NumaArena                   arena(4, 256);
  std::vector<PooledBuffer *> allocated;

  for (size_t i = 0; i < 4; ++i) {
    auto *b = arena.allocate();
    ASSERT_NE(b, nullptr);
    allocated.push_back(b);
  }

  EXPECT_EQ(arena.available(), 0u);
  EXPECT_EQ(arena.in_flight(), 4u);
  EXPECT_EQ(arena.exhaustion_count(), 0u);

  EXPECT_EQ(arena.allocate(), nullptr);
  EXPECT_EQ(arena.allocate(), nullptr);
  EXPECT_EQ(arena.exhaustion_count(), 2u);

  for (auto *b : allocated) {
    b->release();
  }
  EXPECT_EQ(arena.available(), 4u);
  EXPECT_EQ(arena.in_flight(), 0u);
}

TEST(NumaArenaTest, OverflowDimensionsRejectedCleanly) {
  NumaArena zero_arena(0, 2048);
  EXPECT_EQ(zero_arena.total_capacity(), 0u);
  EXPECT_EQ(zero_arena.allocate(), nullptr);

  NumaArena huge_arena(std::numeric_limits<size_t>::max() / 2, 4096);
  EXPECT_EQ(huge_arena.total_capacity(), 0u);
  EXPECT_EQ(huge_arena.allocate(), nullptr);
}

TEST(NumaArenaTest, OwnerThreadMagazineAndLockFreeCrossThreadRecycle) {
  NumaArena                   arena(256, 256);
  rivet::test::ArenaLeakGuard guard(arena);
  arena.bind_owner_thread();

  constexpr size_t            kTotalItems = 10000;
  std::vector<PooledBuffer *> ring(256, nullptr);
  std::atomic<size_t>         produced{0};
  std::atomic<size_t>         consumed{0};

  /// Consumer thread releases slabs back into lock-free Treiber stack while owner allocates
  std::thread consumer([&] {
    size_t idx = 0;
    while (idx < kTotalItems) {
      if (idx < produced.load(std::memory_order_acquire)) {
        PooledBuffer *b = ring[idx % ring.size()];
        if (b != nullptr) {
          ring[idx % ring.size()] = nullptr;
          b->release();
          consumed.store(idx + 1, std::memory_order_release);
          ++idx;
        }
      } else {
        std::this_thread::yield();
      }
    }
  });

  size_t p = 0;
  while (p < kTotalItems) {
    if (p - consumed.load(std::memory_order_acquire) >= 128) {
      std::this_thread::yield();
      continue;
    }
    auto *b = arena.allocate();
    if (!b) {
      std::this_thread::yield();
      continue;
    }
    b->payload_data[0]    = static_cast<std::byte>(p & 0xFF);
    ring[p % ring.size()] = b;
    produced.store(p + 1, std::memory_order_release);
    ++p;
  }

  consumer.join();
  EXPECT_EQ(arena.in_flight(), 0u);
}

TEST(MultiClassArenaTest, TieredSlabSelection256_2048_65536) {
  MultiClassArena arena(32, 4, 16, false);

  auto *s_small = arena.allocate_for_size(120);
  ASSERT_NE(s_small, nullptr);
  EXPECT_EQ(s_small->capacity, MultiClassArena::SMALL_SLAB_BYTES);

  auto *s_mtu = arena.allocate_for_size(1400);
  ASSERT_NE(s_mtu, nullptr);
  EXPECT_EQ(s_mtu->capacity, MultiClassArena::STANDARD_SLAB_BYTES);

  auto *s_jumbo = arena.allocate_for_size(60000);
  ASSERT_NE(s_jumbo, nullptr);
  EXPECT_EQ(s_jumbo->capacity, MultiClassArena::JUMBO_SLAB_BYTES);

  EXPECT_EQ(arena.allocate_for_size(70000), nullptr);
  EXPECT_EQ(arena.in_flight(), 3u);

  s_small->release();
  s_mtu->release();
  s_jumbo->release();
  EXPECT_EQ(arena.in_flight(), 0u);
}

TEST(NumaArenaTest, ArenaLeakGuardReportsZeroLeaks) {
  NumaArena arena(16, 512);
  {
    rivet::test::ArenaLeakGuard guard(arena);
    auto                       *b = arena.allocate();
    ASSERT_NE(b, nullptr);
    b->release();
  }
}

/// ============================================================================
/// PacketRef Tests
/// ============================================================================

TEST(PacketRefTest, SizeAndAlignment) {
  EXPECT_EQ(sizeof(PacketRef), 32u);
  EXPECT_EQ(alignof(PacketRef), 32u);
}

TEST(PacketRefTest, DefaultConstructedIsNull) {
  PacketRef ref;
  EXPECT_FALSE(static_cast<bool>(ref));
  EXPECT_EQ(ref.data(), nullptr);
  EXPECT_TRUE(ref.payload().empty());
}

TEST(PacketRefTest, AdoptVersusRetainSemantics) {
  NumaArena arena(8, 2048);
  auto     *slab = arena.allocate();
  ASSERT_NE(slab, nullptr);
  EXPECT_EQ(slab->ref_count.load(), 1u);

  {
    PacketRef pkt =
        PacketRef::adopt(slab, 0, 100, 12345, 90000, 42, 0, pkt::kClosedIrap | pkt::kRef);
    EXPECT_EQ(slab->ref_count.load(), 1u);
    EXPECT_TRUE(pkt.is_keyframe());
    EXPECT_TRUE(pkt.is_idr());
    EXPECT_TRUE(pkt.is_reference());
    EXPECT_FALSE(pkt.is_marker());
    EXPECT_EQ(pkt.sequence, 42u);
    EXPECT_EQ(pkt.rtp_timestamp, 90000u);
    EXPECT_EQ(pkt.arrival_ts, 12345u);
    EXPECT_EQ(pkt.length, 100u);

    {
      PacketRef copy = pkt;
      EXPECT_EQ(slab->ref_count.load(), 2u);
    }
    EXPECT_EQ(slab->ref_count.load(), 1u);
  }
  EXPECT_EQ(arena.available(), 8u);
  EXPECT_EQ(arena.in_flight(), 0u);
}

TEST(PacketRefTest, MoveSemanticsDoesNotChangeRefCount) {
  NumaArena arena(4, 1024);
  auto     *slab = arena.allocate();
  ASSERT_NE(slab, nullptr);

  {
    PacketRef p1 = PacketRef::adopt(slab, 10, 50, 1, 2, 3, 0, pkt::kMarker);
    EXPECT_EQ(slab->ref_count.load(), 1u);
    EXPECT_TRUE(p1.is_marker());

    PacketRef p2 = std::move(p1);
    EXPECT_EQ(slab->ref_count.load(), 1u);
    EXPECT_FALSE(static_cast<bool>(p1));
    EXPECT_TRUE(static_cast<bool>(p2));
    EXPECT_EQ(p2.offset, 10u);
    EXPECT_EQ(p2.length, 50u);
  }

  EXPECT_EQ(arena.in_flight(), 0u);
}

TEST(PacketRefTest, FlagsAndPackedTrackMetadataClassification) {
  NumaArena                   arena(8, 1024);
  rivet::test::ArenaLeakGuard guard(arena);

  {
    PacketRef p0 = PacketRef::adopt(arena.allocate(), 0, 10, 0, 0, 0, 0, pkt::kNone);
    EXPECT_FALSE(p0.is_keyframe());
    EXPECT_FALSE(p0.is_idr());
    EXPECT_FALSE(p0.is_cra());
    EXPECT_FALSE(p0.is_reference());
    EXPECT_FALSE(p0.is_marker());
    EXPECT_FALSE(p0.is_rasl());

    PacketRef p_idr =
        PacketRef::adopt(arena.allocate(), 0, 10, 0, 0, 0, 0, pkt::kClosedIrap | pkt::kRef);
    EXPECT_TRUE(p_idr.is_keyframe());
    EXPECT_TRUE(p_idr.is_idr());
    EXPECT_TRUE(p_idr.is_reference());

    PacketRef p_cra =
        PacketRef::adopt(arena.allocate(), 0, 10, 0, 0, 0, 0, pkt::kOpenIrap | pkt::kRef);
    EXPECT_TRUE(p_cra.is_keyframe());
    EXPECT_TRUE(p_cra.is_cra());

    PacketRef p_marker = PacketRef::adopt(arena.allocate(), 0, 10, 0, 0, 0, 0, pkt::kMarker);
    EXPECT_TRUE(p_marker.is_marker());

    PacketRef p_rasl = PacketRef::adopt(arena.allocate(), 0, 10, 0, 0, 0, 0, pkt::kRasl);
    EXPECT_TRUE(p_rasl.is_rasl());

    const uint8_t packed = pkt::pack_track_meta(2, 3, true, true);
    PacketRef     p_hevc =
        PacketRef::adopt(arena.allocate(), 0, 10, 0, 0, 0, packed, pkt::kParamSet | pkt::kAuStart);
    EXPECT_EQ(p_hevc.track(), 2u);
    EXPECT_EQ(p_hevc.temporal_id(), 3u);
    EXPECT_TRUE(p_hevc.is_vcl());
    EXPECT_TRUE(p_hevc.is_switch_up());
    EXPECT_TRUE(p_hevc.is_param_set());
    EXPECT_TRUE(p_hevc.is_au_start());
  }

  EXPECT_EQ(arena.in_flight(), 0u);
}

/// ============================================================================
/// MemoryPool Tests
/// ============================================================================

struct DummyItem {
  int x{42};
};

TEST(MemoryPoolTest, AllocateAndDeallocate) {
  MemoryPool<DummyItem> pool(4);
  EXPECT_EQ(pool.capacity(), 4u);
  EXPECT_EQ(pool.available(), 4u);

  DummyItem *item1 = pool.allocate();
  ASSERT_NE(item1, nullptr);
  EXPECT_EQ(item1->x, 42);
  EXPECT_EQ(pool.available(), 3u);

  DummyItem *item2 = pool.allocate();
  DummyItem *item3 = pool.allocate();
  DummyItem *item4 = pool.allocate();
  EXPECT_EQ(pool.available(), 0u);
  EXPECT_EQ(pool.allocate(), nullptr);

  pool.deallocate(item1);
  EXPECT_EQ(pool.available(), 1u);
  DummyItem *reclaimed = pool.allocate();
  EXPECT_EQ(reclaimed, item1);

  pool.deallocate(item2);
  pool.deallocate(item3);
  pool.deallocate(item4);
  pool.deallocate(reclaimed);
  EXPECT_EQ(pool.available(), 4u);
}
