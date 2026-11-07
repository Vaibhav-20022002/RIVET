#pragma once

#include "PacketFlags.hpp"
#include "PooledBuffer.hpp"
#include <cstdint>
#include <span>

namespace rivet::memory {

/// A lightweight, strictly 32-byte RAII handle representing a single RTP packet (or sub-slice)
/// stored within a shared PooledBuffer.
struct alignas(32) PacketRef {
  PooledBuffer *buffer{nullptr};  //< 8 bytes: Intrusive slab pointer (0...7)
  uint32_t      length{0};        //< 4 bytes: Length of RTP packet (8...11)
  uint16_t      offset{0};        //< 2 bytes: Offset to RTP packet start (12...13)
  uint16_t      generation_id{0}; //< 2 bytes: ConfigGeneration id stamped at ingest (14...15)
  uint64_t      arrival_ts{0};    //< 8 bytes: Monotonic arrival timestamp (16...23)
  uint32_t      rtp_timestamp{0}; //< 4 bytes: Extracted RTP timestamp (24...27)
  uint16_t      sequence{0};      //< 2 bytes: RTP sequence number (28...29)
  uint8_t       track_id{0};      //< 1 byte: Packed [switch_up|vcl|tid(3)|track(3)] (30)
  uint8_t       flags{0};         //< 1 byte: memory::pkt::k* bitmask (31)

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
      , length(len)
      , offset(static_cast<uint16_t>(off))
      , arrival_ts(arr_ts)
      , rtp_timestamp(rtp_ts)
      , sequence(seq)
      , track_id(track)
      , flags(fl) {}

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
      , length(len)
      , offset(static_cast<uint16_t>(off))
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
      , length(other.length)
      , offset(other.offset)
      , generation_id(other.generation_id)
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
      , length(other.length)
      , offset(other.offset)
      , generation_id(other.generation_id)
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
      length        = other.length;
      offset        = other.offset;
      generation_id = other.generation_id;
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
      length        = other.length;
      offset        = other.offset;
      generation_id = other.generation_id;
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

  /// Byte-31 flag queries
  [[nodiscard]] bool is_keyframe() const noexcept { return (flags & pkt::kIrapMask) != 0; }
  [[nodiscard]] bool is_rap() const noexcept { return is_keyframe(); }
  [[nodiscard]] bool is_idr() const noexcept { return (flags & pkt::kClosedIrap) != 0; }
  [[nodiscard]] bool is_cra() const noexcept { return (flags & pkt::kOpenIrap) != 0; }
  [[nodiscard]] bool is_open_rap() const noexcept { return is_cra(); }
  [[nodiscard]] bool is_reference() const noexcept { return (flags & pkt::kRef) != 0; }
  [[nodiscard]] bool is_marker() const noexcept { return (flags & pkt::kMarker) != 0; }
  [[nodiscard]] bool has_marker() const noexcept { return is_marker(); }
  [[nodiscard]] bool is_rasl() const noexcept { return (flags & pkt::kRasl) != 0; }
  [[nodiscard]] bool is_radl() const noexcept { return (flags & pkt::kRadl) != 0; }
  [[nodiscard]] bool is_param_set() const noexcept { return (flags & pkt::kParamSet) != 0; }
  [[nodiscard]] bool is_au_start() const noexcept { return (flags & pkt::kAuStart) != 0; }

  /// Byte-30 packed track & HEVC TemporalId queries
  [[nodiscard]] uint8_t track() const noexcept { return pkt::unpack_track(track_id); }
  [[nodiscard]] uint8_t temporal_id() const noexcept { return pkt::unpack_temporal_id(track_id); }
  [[nodiscard]] bool    is_vcl() const noexcept { return pkt::unpack_vcl(track_id); }
  [[nodiscard]] bool    is_switch_up() const noexcept { return pkt::unpack_switch_up(track_id); }
  void                  set_switch_up(bool v) noexcept {
    track_id = v ? static_cast<uint8_t>(track_id | 0x80U)
                 : static_cast<uint8_t>(track_id & ~uint8_t{0x80U});
  }
  [[nodiscard]] bool is_sublayer_nonref() const noexcept { return is_vcl() && !is_reference(); }

  /// Computes RTP fixed + CSRC + RFC 5285/3550 extension header length in bytes (0 if invalid).
  [[nodiscard]] uint32_t rtp_header_length() const noexcept {
    auto span = payload();
    if (span.size() < 12) return 0;
    const auto   *p       = reinterpret_cast<const uint8_t *>(span.data());
    const uint8_t cc      = p[0] & 0x0FU;
    const bool    has_ext = (p[0] & 0x10U) != 0;
    uint32_t      hlen    = 12U + (static_cast<uint32_t>(cc) * 4U);
    if (span.size() < hlen) return 0;
    if (has_ext) {
      if (span.size() < hlen + 4U) return 0;
      const uint16_t ext_words =
          static_cast<uint16_t>((static_cast<uint16_t>(p[hlen + 2]) << 8U) | p[hlen + 3]);
      hlen += 4U + (static_cast<uint32_t>(ext_words) * 4U);
      if (span.size() < hlen) return 0;
    }
    return hlen;
  }

  /// Returns a bounds-checked span of the codec RTP payload (excluding RTP header & RTP padding).
  [[nodiscard]] std::span<const std::byte> rtp_payload() const noexcept {
    auto full = payload();
    if (full.size() < 12) return {};
    const uint32_t hlen = rtp_header_length();
    if (hlen == 0 || hlen > full.size()) return {};
    size_t      plen = full.size() - hlen;
    const auto *p    = reinterpret_cast<const uint8_t *>(full.data());
    if ((p[0] & 0x20U) != 0) {
      if (plen == 0) return {};
      const uint8_t pad = p[full.size() - 1];
      if (pad == 0 || pad > plen) return {};
      plen -= pad;
    }
    return full.subspan(hlen, plen);
  }

  /// Direct pointer to packet data
  [[nodiscard]] const void *data() const noexcept {
    return buffer ? reinterpret_cast<const void *>(buffer->data() + offset) : nullptr;
  }

  /// Returns a bounds-checked span representing the exact wire memory of this RTP packet
  [[nodiscard]] std::span<const std::byte> payload() const noexcept {
    return buffer ? std::span<const std::byte>{buffer->data() + offset, length}
                  : std::span<const std::byte>{};
  }
};

/// Compile-time static assertions to guarantee sizing layout
static_assert(sizeof(PooledBuffer *) == 8, "Expected 64-bit pointer");
static_assert(sizeof(PacketRef) == 32, "PacketRef must be exactly 32 bytes");

} //< namespace rivet::memory
