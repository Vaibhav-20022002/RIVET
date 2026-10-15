#pragma once

#include "PooledBuffer.hpp"
#include <cstdint>
#include <span>

namespace rivet::memory {

/// A lightweight, strictly 32-byte RAII handle representing a single RTP packet (or sub-slice)
/// stored within a shared PooledBuffer.
struct alignas(32) PacketRef {
  PooledBuffer *buffer{nullptr};  //< 8 bytes: Intrusive slab pointer (0...7)
  uint32_t      offset{0};        //< 4 bytes: Offset to RTP packet start (8...11)
  uint32_t      length{0};        //< 4 bytes: Length of RTP packet (12...15)
  uint64_t      arrival_ts{0};    //< 8 bytes: Monotonic arrival timestamp (16...23)
  uint32_t      rtp_timestamp{0}; //< 4 bytes: Extracted RTP timestamp (24...27)
  uint16_t      sequence{0};      //< 2 bytes: RTP sequence number (28...29)
  uint8_t       track_id{0};      //< 1 byte: 0 = Video, 1 = Audio (30)
  uint8_t       flags{
      0}; //< 1 byte: Bit 0: IDR, Bit 1: CRA, Bit 2: Ref Pic, Bit 3: Marker, Bit 4: RASL (31)

  /// Tag type for adopting an existing reference without incrementing ref_count
  struct adopt_ref_t {
    explicit adopt_ref_t() = default;
  };

  static constexpr adopt_ref_t adopt_ref{};

  /// Default constructor: empty/null handle
  PacketRef() noexcept = default;

  /// Adopting constructor: Takes ownership of an existing reference (e.g. freshly allocated
  /// buffer). Does NOT call retain(), transferring ownership directly into the PacketRef.
  explicit PacketRef(adopt_ref_t,
      PooledBuffer *buff,
      uint32_t      off,
      uint32_t      len,
      uint64_t      arr_ts,
      uint32_t      rtp_ts,
      uint16_t      seq,
      uint8_t       track,
      uint8_t       fl) noexcept
      : buffer(buff)
      , offset(off)
      , length(len)
      , arrival_ts(arr_ts)
      , rtp_timestamp(rtp_ts)
      , sequence(seq)
      , track_id(track)
      , flags(fl) {
    /// Ownership adopted: ref_count remains unchanged
  }

  /// Retaining constructor: Binds to an existing buffer and increments ref_count
  explicit PacketRef(PooledBuffer *buff,
      uint32_t                     off,
      uint32_t                     len,
      uint64_t                     arr_ts,
      uint32_t                     rtp_ts,
      uint16_t                     seq,
      uint8_t                      track,
      uint8_t                      fl) noexcept
      : buffer(buff)
      , offset(off)
      , length(len)
      , arrival_ts(arr_ts)
      , rtp_timestamp(rtp_ts)
      , sequence(seq)
      , track_id(track)
      , flags(fl) {
    if (buffer) buffer->retain();
  }

  /// Named factory function for unambiguously adopting a reference
  static PacketRef adopt(PooledBuffer *buff,
      uint32_t                         off,
      uint32_t                         len,
      uint64_t                         arr_ts,
      uint32_t                         rtp_ts,
      uint16_t                         seq,
      uint8_t                          track,
      uint8_t                          fl) noexcept {
    return PacketRef(adopt_ref, buff, off, len, arr_ts, rtp_ts, seq, track, fl);
  }

  /// Named factory function for unambiguously retaining a reference
  static PacketRef retain(PooledBuffer *buff,
      uint32_t                          off,
      uint32_t                          len,
      uint64_t                          arr_ts,
      uint32_t                          rtp_ts,
      uint16_t                          seq,
      uint8_t                           track,
      uint8_t                           fl) noexcept {
    return PacketRef(buff, off, len, arr_ts, rtp_ts, seq, track, fl);
  }

  /// Destructor: Automatically decrements intrusive ref_count upon destruction
  ~PacketRef() noexcept { reset(); }

  /// Copy Constructor: Increments ref_count on shared buffer
  PacketRef(const PacketRef &other) noexcept
      : buffer(other.buffer)
      , offset(other.offset)
      , length(other.length)
      , arrival_ts(other.arrival_ts)
      , rtp_timestamp(other.rtp_timestamp)
      , sequence(other.sequence)
      , track_id(other.track_id)
      , flags(other.flags) {
    if (buffer) {
      buffer->retain();
    }
  }

  /// Move Constructor: Zero-overhead transfer of ownership without atomic operations
  PacketRef(PacketRef &&other) noexcept
      : buffer(other.buffer)
      , offset(other.offset)
      , length(other.length)
      , arrival_ts(other.arrival_ts)
      , rtp_timestamp(other.rtp_timestamp)
      , sequence(other.sequence)
      , track_id(other.track_id)
      , flags(other.flags) {
    other.buffer = nullptr;
  }

  /// Copy Assignment: Releases current buffer and retains new buffer
  PacketRef &operator=(const PacketRef &other) noexcept {
    if (this != &other) {
      reset();
      buffer        = other.buffer;
      offset        = other.offset;
      length        = other.length;
      arrival_ts    = other.arrival_ts;
      rtp_timestamp = other.rtp_timestamp;
      sequence      = other.sequence;
      track_id      = other.track_id;
      flags         = other.flags;
      if (buffer) {
        buffer->retain();
      }
    }
    return *this;
  }

  /// Move Assignment: Releases current buffer and steals pointer without atomic overhead
  PacketRef &operator=(PacketRef &&other) noexcept {
    if (this != &other) {
      reset();
      buffer        = other.buffer;
      offset        = other.offset;
      length        = other.length;
      arrival_ts    = other.arrival_ts;
      rtp_timestamp = other.rtp_timestamp;
      sequence      = other.sequence;
      track_id      = other.track_id;
      flags         = other.flags;
      other.buffer  = nullptr;
    }
    return *this;
  }

  void reset() noexcept {
    if (buffer) {
      buffer->release();
      buffer = nullptr;
    }
  }

  [[nodiscard]] explicit operator bool() const noexcept { return buffer != nullptr; }

  [[nodiscard]] bool is_keyframe() const noexcept { return (flags & 0x03) != 0; }

  [[nodiscard]] bool is_idr() const noexcept { return (flags & 0x01) != 0; }

  [[nodiscard]] bool is_cra() const noexcept { return (flags & 0x02) != 0; }

  [[nodiscard]] bool is_reference() const noexcept { return (flags & 0x04) != 0; }

  [[nodiscard]] bool is_marker() const noexcept { return (flags & 0x08) != 0; }

  [[nodiscard]] bool is_rasl() const noexcept { return (flags & 0x10) != 0; }

  /// Returns a bounds-checked span representing the exact memory of this packet
  [[nodiscard]] std::span<const std::byte> payload() const noexcept {
    return buffer ? std::span<const std::byte>{buffer->data() + offset, length}
                  : std::span<const std::byte>{};
  }
};

/// Compile-time static assertions to guarantee sizing layout
static_assert(sizeof(PooledBuffer *) == 8, "Expected 64-bit pointer");
static_assert(sizeof(PacketRef) == 32, "PacketRef must be exactly 32 bytes");

} //< namespace rivet::memory