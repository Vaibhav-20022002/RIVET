#include "rivet/memory/MemoryPool.hpp"
#include "rivet/memory/NumaArena.hpp"
#include "rivet/memory/PacketRef.hpp"
#include "rivet/memory/PooledBuffer.hpp"
#include "tests/support/ArenaLeakGuard.hpp"
#include <gtest/gtest.h>
#include <vector>

using namespace rivet::memory;

/// ============================================================================
/// PooledBuffer Tests
/// ============================================================================

TEST(PooledBufferTest, InitialStateAndRetainRelease) {
  alignas(64) uint8_t raw[128]{};
  PooledBuffer        buf;
  buf.capacity     = 128;
  buf.payload_data = reinterpret_cast<std::byte *>(raw);

  buf.recycle_fn = [](PooledBuffer *b) noexcept {
    *reinterpret_cast<bool *>(b->payload_data) = true;
  };
  *reinterpret_cast<bool *>(raw) = false;

  buf.ref_count.store(1, std::memory_order_relaxed);
  EXPECT_EQ(buf.ref_count.load(), 1u);
  EXPECT_EQ(buf.size(), 128u);
  EXPECT_EQ(buf.data(), reinterpret_cast<std::byte *>(raw));

  buf.retain();
  EXPECT_EQ(buf.ref_count.load(), 2u);

  buf.release();
  EXPECT_EQ(buf.ref_count.load(), 1u);
  EXPECT_FALSE(*reinterpret_cast<bool *>(raw));

  buf.release();
  EXPECT_EQ(buf.ref_count.load(), 0u);
  EXPECT_TRUE(*reinterpret_cast<bool *>(raw));
}

/// ============================================================================
/// NumaArena Tests
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

  /// Check 64-byte alignment of the payload data
  auto addr = reinterpret_cast<uintptr_t>(slab->payload_data);
  EXPECT_EQ(addr % 64, 0u);

  slab->release();
  EXPECT_EQ(arena.available(), 64u);
  EXPECT_EQ(arena.in_flight(), 0u);
}

TEST(NumaArenaTest, ExhaustionReturnsNullptr) {
  NumaArena                   arena(4, 256);
  std::vector<PooledBuffer *> allocated;

  for (size_t i = 0; i < 4; ++i) {
    auto *b = arena.allocate();
    ASSERT_NE(b, nullptr);
    allocated.push_back(b);
  }

  EXPECT_EQ(arena.available(), 0u);
  EXPECT_EQ(arena.in_flight(), 4u);

  /// Next allocation must return nullptr
  EXPECT_EQ(arena.allocate(), nullptr);

  /// Release all
  for (auto *b : allocated) {
    b->release();
  }
  EXPECT_EQ(arena.available(), 4u);
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
  /// Slab was allocated with ref_count = 1
  EXPECT_EQ(slab->ref_count.load(), 1u);

  {
    /// Adopt should NOT increment ref_count (it takes ownership of the initial ref)
    PacketRef pkt = PacketRef::adopt(slab, 0, 100, 12345, 90000, 42, 0, 0x05);
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
      /// Copy constructor MUST retain and increment ref_count
      PacketRef copy = pkt;
      EXPECT_EQ(slab->ref_count.load(), 2u);
    }
    /// Copy destroyed, ref_count returns to 1
    EXPECT_EQ(slab->ref_count.load(), 1u);
  }
  /// Original pkt destroyed, slab recycled back to arena!
  EXPECT_EQ(arena.available(), 8u);
  EXPECT_EQ(arena.in_flight(), 0u);
}

TEST(PacketRefTest, MoveSemanticsDoesNotChangeRefCount) {
  NumaArena arena(4, 1024);
  auto     *slab = arena.allocate();
  ASSERT_NE(slab, nullptr);

  {
    PacketRef p1 = PacketRef::adopt(slab, 10, 50, 1, 2, 3, 0, 0x08);
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

TEST(PacketRefTest, FlagsClassification) {
  NumaArena arena(4, 1024);
  auto     *slab = arena.allocate();

  PacketRef p0 = PacketRef::adopt(slab, 0, 10, 0, 0, 0, 0, 0x00);
  EXPECT_FALSE(p0.is_keyframe());
  EXPECT_FALSE(p0.is_idr());
  EXPECT_FALSE(p0.is_cra());
  EXPECT_FALSE(p0.is_reference());
  EXPECT_FALSE(p0.is_marker());
  EXPECT_FALSE(p0.is_rasl());

  slab->ref_count.store(1, std::memory_order_relaxed);
  PacketRef p_idr = PacketRef::adopt(slab, 0, 10, 0, 0, 0, 0, 0x01 | 0x04);
  EXPECT_TRUE(p_idr.is_keyframe());
  EXPECT_TRUE(p_idr.is_idr());
  EXPECT_TRUE(p_idr.is_reference());

  slab->ref_count.store(1, std::memory_order_relaxed);
  PacketRef p_cra = PacketRef::adopt(slab, 0, 10, 0, 0, 0, 0, 0x02 | 0x04);
  EXPECT_TRUE(p_cra.is_keyframe());
  EXPECT_TRUE(p_cra.is_cra());

  slab->ref_count.store(1, std::memory_order_relaxed);
  PacketRef p_marker = PacketRef::adopt(slab, 0, 10, 0, 0, 0, 0, 0x08);
  EXPECT_TRUE(p_marker.is_marker());

  slab->ref_count.store(1, std::memory_order_relaxed);
  PacketRef p_rasl = PacketRef::adopt(slab, 0, 10, 0, 0, 0, 0, 0x10);
  EXPECT_TRUE(p_rasl.is_rasl());
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

  /// Pool exhausted
  EXPECT_EQ(pool.allocate(), nullptr);

  /// Deallocate and re-allocate
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
