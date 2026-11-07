#pragma once

#include "rivet/memory/PacketFlags.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace rivet::media::hevc {

/// ============================================================================
/// 1. 64-Entry Compile-Time HEVC NAL Type Lookup Table (RFC 7798 / H.265 Table 7-1)
/// ============================================================================
inline constexpr uint8_t kVcl      = 0x01;
inline constexpr uint8_t kSwitchUp = 0x02;

struct NalInfo {
  uint8_t flags;
  uint8_t attr;
};

consteval std::array<NalInfo, 64> make_lut() {
  using namespace memory::pkt;
  std::array<NalInfo, 64> t{}; //< 35..47 remain {0, 0}: AUD, EOS, EOB, FD, SEI, RSV_NVCL
  for (int i = 0; i <= 15; ++i) {
    t[static_cast<size_t>(i)] = {static_cast<uint8_t>((i & 1) ? kRef : 0), kVcl};
  }
  for (int i : {2, 3, 4, 5}) {
    t[static_cast<size_t>(i)].attr |= kSwitchUp; //< TSA_N/R, STSA_N/R
  }
  for (int i : {6, 7}) {
    t[static_cast<size_t>(i)].flags |= kRadl; //< RADL_N, RADL_R
  }
  for (int i : {8, 9}) {
    t[static_cast<size_t>(i)].flags |= kRasl; //< RASL_N, RASL_R
  }
  for (int i = 16; i <= 31; ++i) {
    t[static_cast<size_t>(i)] = {kRef, kVcl}; //< IRAPs and reserved VCL
  }
  for (int i : {16, 21}) {
    t[static_cast<size_t>(i)].flags |= kOpenIrap; //< BLA_W_LP, CRA_NUT
  }
  for (int i : {17, 18, 19, 20}) {
    t[static_cast<size_t>(i)].flags |= kClosedIrap; //< BLA_W_RADL, BLA_N_LP, IDR_W_RADL, IDR_N_LP
  }
  for (int i : {32, 33, 34}) {
    t[static_cast<size_t>(i)] = {kParamSet, 0}; //< VPS, SPS, PPS
  }
  for (int i = 51; i <= 63; ++i) {
    t[static_cast<size_t>(i)] = {kRef, 0}; //< Unspecified: forward opaquely
  }
  return t;
}

inline constexpr auto kLut = make_lut();

static_assert(kLut[0].flags == 0 && kLut[0].attr == kVcl);
static_assert(kLut[1].flags == memory::pkt::kRef && kLut[1].attr == kVcl);
static_assert(kLut[2].attr == (kVcl | kSwitchUp));
static_assert(kLut[6].flags == memory::pkt::kRadl);
static_assert(kLut[7].flags == (memory::pkt::kRadl | memory::pkt::kRef));
static_assert(kLut[8].flags == memory::pkt::kRasl);
static_assert(kLut[9].flags == (memory::pkt::kRasl | memory::pkt::kRef));
static_assert(kLut[16].flags == (memory::pkt::kRef | memory::pkt::kOpenIrap));
static_assert(kLut[19].flags == (memory::pkt::kRef | memory::pkt::kClosedIrap));
static_assert(kLut[21].flags == (memory::pkt::kRef | memory::pkt::kOpenIrap));
static_assert(kLut[32].flags == memory::pkt::kParamSet && kLut[32].attr == 0);
static_assert(kLut[33].flags == memory::pkt::kParamSet && kLut[33].attr == 0);
static_assert(kLut[34].flags == memory::pkt::kParamSet && kLut[34].attr == 0);

struct NalHeader {
  uint8_t type;
  uint8_t layer_id;
  uint8_t tid;
  bool    ok;
};

[[nodiscard]] constexpr NalHeader parse_nal_header(uint8_t b0, uint8_t b1) noexcept {
  const uint8_t tid1 = b1 & 0x07U;
  return NalHeader{
      .type     = static_cast<uint8_t>((b0 >> 1U) & 0x3FU),
      .layer_id = static_cast<uint8_t>(((b0 & 0x01U) << 5U) | (b1 >> 3U)),
      .tid      = static_cast<uint8_t>(tid1 - 1U),
      .ok       = ((b0 & 0x80U) == 0U) && (tid1 != 0U) //< RFC 7798 §1.1.4: F == 0 && TID != 0
  };
}

/// ============================================================================
/// 2. RFC 7798 Zero-Allocation Payload Classifier (Single NAL, AP, FU, PACI)
/// ============================================================================
enum class MalformedReason : uint8_t {
  None         = 0,
  BadNalHeader = 1,
  BadAp        = 2,
  BadFu        = 3,
  BadPaci      = 4
};

struct HevcClass {
  uint8_t         flags{0};   //< memory::pkt::* (excluding kMarker / kAuStart)
  uint8_t         tid{7};     //< Lowest TID among VCL NALs; 7 = non-VCL
  uint8_t         ps_mask{0}; //< bit 0: VPS, bit 1: SPS, bit 2: PPS
  bool            vcl{false};
  bool            switch_up{false};
  bool            first_slice{false};
  bool            malformed{false};
  bool            nonbase_layer{false};
  bool            is_fu{false};
  bool            fu_start{false};
  bool            fu_end{false};
  MalformedReason reason{MalformedReason::None};
};

namespace detail {

inline void apply_nal_type(HevcClass &out, uint8_t nal_type, uint8_t nal_tid) noexcept {
  const NalInfo info = kLut[nal_type & 0x3FU];
  out.flags |= info.flags;
  if ((info.attr & kVcl) != 0) {
    out.vcl = true;
    if (nal_tid < out.tid) {
      out.tid = nal_tid;
    }
    if ((info.attr & kSwitchUp) != 0) {
      out.switch_up = true;
    }
  }
  if (nal_type >= 32U && nal_type <= 34U) {
    out.ps_mask |= static_cast<uint8_t>(1U << (nal_type - 32U));
  }
}

/// Classifies an inner structure whose virtual 2-byte NAL header has already been decoded as
/// `hdr`, and whose body (bytes after those 2 octets) begins at `body[0..body_len)`.
inline void classify_structure(HevcClass &out,
    const NalHeader                      &hdr,
    const uint8_t                        *body,
    size_t                                body_len,
    bool                                  don) noexcept {
  using namespace memory::pkt;

  if (hdr.type < 48U) {
    /// Single NAL Unit (RFC 7798 §4.4.1)
    const size_t don_bytes = don ? 2U : 0U;
    if (body_len < don_bytes) {
      out.malformed = true;
      out.reason    = MalformedReason::BadNalHeader;
      out.flags     = kRef;
      return;
    }
    apply_nal_type(out, hdr.type, hdr.tid);
    if ((kLut[hdr.type].attr & kVcl) != 0 && body_len > don_bytes) {
      out.first_slice = (body[don_bytes] & 0x80U) != 0;
    }
    return;
  }

  if (hdr.type == 48U) {
    /// Aggregation Packet (AP, RFC 7798 §4.4.2)
    size_t off   = 0;
    size_t units = 0;
    while (off < body_len) {
      const size_t d_skip = don ? (units == 0 ? 2U : 1U) : 0U;
      if (off + d_skip + 2U > body_len) {
        out.malformed = true;
        out.reason    = MalformedReason::BadAp;
        if (units == 0) out.flags |= kRef;
        return;
      }
      off += d_skip;
      const uint16_t nalu_size =
          static_cast<uint16_t>((static_cast<uint16_t>(body[off]) << 8U) | body[off + 1U]);
      off += 2U;
      if (nalu_size < 2U || off + nalu_size > body_len) {
        out.malformed = true;
        out.reason    = MalformedReason::BadAp;
        if (units == 0) out.flags |= kRef;
        return;
      }

      const NalHeader sub_hdr = parse_nal_header(body[off], body[off + 1U]);
      if (!sub_hdr.ok || sub_hdr.type >= 48U) {
        out.malformed = true;
        out.reason    = MalformedReason::BadAp;
        if (units == 0) out.flags |= kRef;
        return;
      }
      if (sub_hdr.layer_id == 0U) {
        apply_nal_type(out, sub_hdr.type, sub_hdr.tid);
        if ((kLut[sub_hdr.type].attr & kVcl) != 0 && nalu_size > 2U && units == 0) {
          out.first_slice = (body[off + 2U] & 0x80U) != 0;
        }
      } else {
        out.nonbase_layer = true;
        out.flags |= kRef;
      }
      off += nalu_size;
      ++units;
    }

    if (units < 2U) {
      out.malformed = true;
      out.reason    = MalformedReason::BadAp;
      if (units == 0) out.flags |= kRef;
    }
    return;
  }

  if (hdr.type == 49U) {
    /// Fragmentation Unit (FU, RFC 7798 §4.4.3)
    if (body_len < 1U) {
      out.malformed = true;
      out.reason    = MalformedReason::BadFu;
      out.flags     = kRef;
      return;
    }
    const uint8_t fu_hdr  = body[0];
    const bool    s_bit   = (fu_hdr & 0x80U) != 0;
    const bool    e_bit   = (fu_hdr & 0x40U) != 0;
    const uint8_t fu_type = fu_hdr & 0x3FU;

    const size_t min_body = 1U + ((s_bit && don) ? 2U : 0U) + 1U; //< >= 1 fragment payload byte
    if ((s_bit && e_bit) || fu_type >= 48U || body_len < min_body) {
      out.malformed = true;
      out.reason    = MalformedReason::BadFu;
      out.flags     = kRef;
      return;
    }

    out.is_fu    = true;
    out.fu_start = s_bit;
    out.fu_end   = e_bit;

    apply_nal_type(out, fu_type, hdr.tid);
    /// IRAP bits apply strictly to the start fragment (S == 1) so RapEntry indexes AU start
    if (!s_bit) {
      out.flags = static_cast<uint8_t>(out.flags & ~kIrapMask);
    } else if ((kLut[fu_type].attr & kVcl) != 0) {
      const size_t slice_off = 1U + (don ? 2U : 0U);
      out.first_slice        = (body[slice_off] & 0x80U) != 0;
    }
    return;
  }

  /// Unspecified NAL types 51..63
  apply_nal_type(out, hdr.type, hdr.tid);
}

} //< namespace detail

[[nodiscard]] inline HevcClass classify(const uint8_t *p, size_t n, bool don = false) noexcept {
  using namespace memory::pkt;
  HevcClass out{};

  if (!p || n < 2U) {
    out.malformed = true;
    out.reason    = MalformedReason::BadNalHeader;
    out.flags     = kRef;
    return out;
  }

  const NalHeader hdr = parse_nal_header(p[0], p[1]);
  if (!hdr.ok) {
    out.malformed = true;
    out.reason    = MalformedReason::BadNalHeader;
    out.flags     = kRef;
    return out;
  }

  if (hdr.layer_id > 0U) {
    /// Non-goal N2: multi-layer SHVC / MV-HEVC forwarded opaquely
    out.nonbase_layer = true;
    out.flags         = kRef;
    return out;
  }

  if (hdr.type == 50U) {
    /// PACI (RFC 7798 §4.4.4)
    if (n < 4U) {
      out.malformed = true;
      out.reason    = MalformedReason::BadPaci;
      out.flags     = kRef;
      return out;
    }
    const uint8_t b2       = p[2];
    const uint8_t b3       = p[3];
    const bool    a_bit    = (b2 & 0x80U) != 0;
    const uint8_t ctype    = static_cast<uint8_t>((b2 >> 1U) & 0x3FU);
    const uint8_t phs_size = static_cast<uint8_t>(((b2 & 0x01U) << 4U) | (b3 >> 4U));
    const size_t  inner    = 4U + static_cast<size_t>(phs_size);

    if (a_bit || ctype == 50U || inner > n) {
      out.malformed = true;
      out.reason    = MalformedReason::BadPaci;
      out.flags     = kRef;
      return out;
    }

    const NalHeader virtual_hdr{
        .type = ctype, .layer_id = hdr.layer_id, .tid = hdr.tid, .ok = true};
    detail::classify_structure(out, virtual_hdr, p + inner, n - inner, don);
    return out;
  }

  detail::classify_structure(out, hdr, p + 2U, n - 2U, don);
  return out;
}

/// ============================================================================
/// 3. Parameter-Set Extraction & Bounded FU Reassembler
/// ============================================================================
template<typename Callback>
inline void for_each_parameter_set(const uint8_t *p, size_t n, bool don, Callback &&cb) noexcept {
  if (!p || n < 2U) return;
  const NalHeader hdr = parse_nal_header(p[0], p[1]);
  if (!hdr.ok || hdr.layer_id > 0U) return;

  const uint8_t *body     = p + 2U;
  size_t         body_len = n - 2U;
  uint8_t        type     = hdr.type;

  if (type == 50U) {
    if (n < 4U) return;
    const bool    a_bit    = (p[2] & 0x80U) != 0;
    const uint8_t ctype    = static_cast<uint8_t>((p[2] >> 1U) & 0x3FU);
    const uint8_t phs_size = static_cast<uint8_t>(((p[2] & 0x01U) << 4U) | (p[3] >> 4U));
    const size_t  inner    = 4U + static_cast<size_t>(phs_size);
    if (a_bit || ctype == 50U || inner > n) return;
    type     = ctype;
    body     = p + inner;
    body_len = n - inner;
  }

  if (type >= 32U && type <= 34U) {
    const size_t d_skip = don ? 2U : 0U;
    if (body_len < d_skip) return;
    if (hdr.type == 50U) {
      /// Reconstruct 2-byte NAL header stripped by PACI
      std::array<uint8_t, 4096> scratch{};
      const size_t              nal_body_len = body_len - d_skip;
      if (2U + nal_body_len > scratch.size()) return;
      scratch[0] = static_cast<uint8_t>((type << 1U) | (p[0] & 0x01U));
      scratch[1] = p[1];
      std::memcpy(scratch.data() + 2U, body + d_skip, nal_body_len);
      cb(type, scratch.data(), 2U + nal_body_len);
    } else if (!don) {
      cb(type, p, n);
    } else {
      std::array<uint8_t, 4096> scratch{};
      const size_t              nal_body_len = body_len - 2U;
      if (2U + nal_body_len > scratch.size()) return;
      scratch[0] = p[0];
      scratch[1] = p[1];
      std::memcpy(scratch.data() + 2U, body + 2U, nal_body_len);
      cb(type, scratch.data(), 2U + nal_body_len);
    }
    return;
  }

  if (type == 48U) {
    size_t off   = 0;
    size_t units = 0;
    while (off < body_len) {
      const size_t d_skip = don ? (units == 0 ? 2U : 1U) : 0U;
      if (off + d_skip + 2U > body_len) break;
      off += d_skip;
      const uint16_t sz =
          static_cast<uint16_t>((static_cast<uint16_t>(body[off]) << 8U) | body[off + 1U]);
      off += 2U;
      if (sz < 2U || off + sz > body_len) break;
      const NalHeader sub = parse_nal_header(body[off], body[off + 1U]);
      if (!sub.ok || sub.type >= 48U) break;
      if (sub.layer_id == 0U && sub.type >= 32U && sub.type <= 34U) {
        cb(sub.type, body + off, sz);
      }
      off += sz;
      ++units;
    }
  }
}

/// Bounded 4 KiB scratch reassembler for rare FU-fragmented VPS/SPS/PPS.
/// Discards in-progress state on any sequence gap or > 4 KiB overflow.
class FuParamReassembler {
public:
  static constexpr size_t MAX_PARAM_SET_BYTES = 4096;

private:
  std::array<uint8_t, MAX_PARAM_SET_BYTES> buf_{};
  size_t                                   len_{0};
  uint16_t                                 expected_seq_{0};
  uint8_t                                  nal_type_{0};
  bool                                     active_{false};

public:
  void reset() noexcept {
    active_ = false;
    len_    = 0;
  }

  /// Feeds an RTP payload carrying an HEVC FU (type 49) or PACI-wrapped FU.
  /// Invokes `cb(nal_type, ptr, len)` when a complete <= 4 KiB parameter set finishes,
  /// and returns `false` if discarded due to sequence gap or > 4 KiB overflow.
  template<typename Callback>
  bool
  on_fu_packet(const uint8_t *p, size_t n, uint16_t rtp_seq, bool don, Callback &&cb) noexcept {
    if (!p || n < 3U) return true;
    const NalHeader hdr = parse_nal_header(p[0], p[1]);
    if (!hdr.ok || hdr.layer_id > 0U) return true;

    const uint8_t *body     = p + 2U;
    size_t         body_len = n - 2U;
    if (hdr.type == 50U) {
      if (n < 5U) return true;
      const uint8_t ctype    = static_cast<uint8_t>((p[2] >> 1U) & 0x3FU);
      const uint8_t phs_size = static_cast<uint8_t>(((p[2] & 0x01U) << 4U) | (p[3] >> 4U));
      const size_t  inner    = 4U + static_cast<size_t>(phs_size);
      if (ctype != 49U || inner >= n) return true;
      body     = p + inner;
      body_len = n - inner;
    } else if (hdr.type != 49U) {
      return true;
    }

    if (body_len < 2U) return true;
    const uint8_t fu_hdr  = body[0];
    const bool    s_bit   = (fu_hdr & 0x80U) != 0;
    const bool    e_bit   = (fu_hdr & 0x40U) != 0;
    const uint8_t fu_type = fu_hdr & 0x3FU;
    if (fu_type < 32U || fu_type > 34U) {
      return true;
    }
    if (s_bit && e_bit) {
      reset();
      return false;
    }

    const size_t frag_off = 1U + ((s_bit && don) ? 2U : 0U);
    if (body_len <= frag_off) {
      reset();
      return false;
    }
    const uint8_t *frag_ptr = body + frag_off;
    const size_t   frag_len = body_len - frag_off;

    if (s_bit) {
      if (2U + frag_len > MAX_PARAM_SET_BYTES) {
        reset();
        return false;
      }
      buf_[0] = static_cast<uint8_t>((fu_type << 1U) | (p[0] & 0x01U));
      buf_[1] = p[1];
      std::memcpy(buf_.data() + 2U, frag_ptr, frag_len);
      len_          = 2U + frag_len;
      nal_type_     = fu_type;
      expected_seq_ = static_cast<uint16_t>(rtp_seq + 1U);
      active_       = true;
      return true;
    }

    if (!active_ || fu_type != nal_type_ || rtp_seq != expected_seq_) {
      const bool had_active = active_;
      reset();
      return !had_active;
    }

    if (len_ + frag_len > MAX_PARAM_SET_BYTES) {
      reset();
      return false;
    }

    std::memcpy(buf_.data() + len_, frag_ptr, frag_len);
    len_ += frag_len;
    expected_seq_ = static_cast<uint16_t>(rtp_seq + 1U);

    if (e_bit) {
      cb(nal_type_, buf_.data(), len_);
      reset();
    }
    return true;
  }
};

/// ============================================================================
/// 4. Emulation-Prevention Removal, BitReader & SPS/VPS/PPS Parsers
/// ============================================================================
class BitReader {
  const uint8_t *data_{nullptr};
  size_t         bit_len_{0};
  size_t         bit_pos_{0};

public:
  BitReader(const uint8_t *data, size_t byte_len) noexcept
      : data_(data)
      , bit_len_(byte_len * 8U) {}

  [[nodiscard]] bool has_bits(size_t count) const noexcept { return bit_pos_ + count <= bit_len_; }

  [[nodiscard]] std::optional<uint64_t> read_bits(size_t count) noexcept {
    if (count > 64U || !has_bits(count)) return std::nullopt;
    uint64_t val = 0;
    for (size_t i = 0; i < count; ++i) {
      const uint8_t byte = data_[bit_pos_ >> 3U];
      const uint8_t bit  = static_cast<uint8_t>((byte >> (7U - (bit_pos_ & 7U))) & 1U);
      val                = (val << 1U) | bit;
      ++bit_pos_;
    }
    return val;
  }

  /// Unsigned Exp-Golomb `ue(v)` bounded to 32-bit values
  [[nodiscard]] std::optional<uint32_t> read_ue() noexcept {
    size_t leading_zeros = 0;
    while (true) {
      auto b = read_bits(1);
      if (!b) return std::nullopt;
      if (*b == 1U) break;
      ++leading_zeros;
      if (leading_zeros > 31U) return std::nullopt;
    }
    if (leading_zeros == 0U) return 0U;
    auto suffix = read_bits(leading_zeros);
    if (!suffix) return std::nullopt;
    return static_cast<uint32_t>(((1ULL << leading_zeros) - 1ULL) + *suffix);
  }
};

/// Strips H.264/H.265 `0x00 00 03` emulation-prevention bytes starting at `start_offset`.
inline size_t strip_emulation_prevention(std::span<const uint8_t> nal,
    size_t                                                        start_offset,
    uint8_t                                                      *out_rbsp,
    size_t                                                        max_rbsp) noexcept {
  size_t n     = 0;
  int    zeros = 0;
  for (size_t i = start_offset; i < nal.size() && n < max_rbsp; ++i) {
    const uint8_t b = nal[i];
    if (zeros >= 2 && b == 0x03U) {
      zeros = 0;
      continue;
    }
    zeros         = (b == 0x00U) ? (zeros + 1) : 0;
    out_rbsp[n++] = b;
  }
  return n;
}

struct Ptl {
  uint8_t  profile_space{0};
  uint8_t  tier_flag{0};
  uint8_t  profile_idc{1};
  uint8_t  level_idc{93};
  uint8_t  max_sub_layers{1};
  uint8_t  sps_id{0};
  uint32_t compat_flags{0};
  uint64_t constraint_flags{0};
};

namespace detail {

[[nodiscard]] inline bool
parse_profile_tier_level_bits(BitReader &br, uint8_t max_sub_layers_minus1, Ptl &out) noexcept {
  auto pspace = br.read_bits(2);
  auto tflag  = br.read_bits(1);
  auto pidc   = br.read_bits(5);
  auto compat = br.read_bits(32);
  auto constr = br.read_bits(48);
  auto lidc   = br.read_bits(8);
  if (!pspace || !tflag || !pidc || !compat || !constr || !lidc) return false;

  out.profile_space    = static_cast<uint8_t>(*pspace);
  out.tier_flag        = static_cast<uint8_t>(*tflag);
  out.profile_idc      = static_cast<uint8_t>(*pidc);
  out.compat_flags     = static_cast<uint32_t>(*compat);
  out.constraint_flags = *constr;
  out.level_idc        = static_cast<uint8_t>(*lidc);

  std::array<bool, 8> sub_profile_present{};
  std::array<bool, 8> sub_level_present{};
  for (uint8_t i = 0; i < max_sub_layers_minus1; ++i) {
    auto sp = br.read_bits(1);
    auto sl = br.read_bits(1);
    if (!sp || !sl) return false;
    sub_profile_present[i] = (*sp != 0);
    sub_level_present[i]   = (*sl != 0);
  }
  if (max_sub_layers_minus1 > 0U) {
    for (uint8_t i = max_sub_layers_minus1; i < 8U; ++i) {
      if (!br.read_bits(2)) return false; //< reserved_zero_2bits
    }
  }
  for (uint8_t i = 0; i < max_sub_layers_minus1; ++i) {
    if (sub_profile_present[i]) {
      if (!br.read_bits(8 + 32 + 48)) return false;
    }
    if (sub_level_present[i]) {
      if (!br.read_bits(8)) return false;
    }
  }
  return true;
}

} //< namespace detail

/// Parses HEVC SPS NAL unit (including 2-byte NAL header) for PTL, `max_sub_layers`, and `sps_id`.
[[nodiscard]] inline std::optional<Ptl> parse_sps_ptl(std::span<const uint8_t> nal) noexcept {
  if (nal.size() < 15U) return std::nullopt;
  const NalHeader hdr = parse_nal_header(nal[0], nal[1]);
  if (!hdr.ok || hdr.type != 33U) return std::nullopt;

  std::array<uint8_t, 256> rbsp{};
  const size_t             n = strip_emulation_prevention(nal, 2U, rbsp.data(), rbsp.size());
  if (n < 13U) return std::nullopt;

  BitReader br(rbsp.data(), n);
  auto      vps_id       = br.read_bits(4);
  auto      max_sub_m1   = br.read_bits(3);
  auto      temp_nesting = br.read_bits(1);
  if (!vps_id || !max_sub_m1 || !temp_nesting || *max_sub_m1 > 6U) return std::nullopt;

  Ptl out{};
  out.max_sub_layers = static_cast<uint8_t>(*max_sub_m1 + 1U);
  if (!detail::parse_profile_tier_level_bits(br, static_cast<uint8_t>(*max_sub_m1), out)) {
    return std::nullopt;
  }

  auto sps_id = br.read_ue();
  if (!sps_id || *sps_id >= 16U) return std::nullopt;
  out.sps_id = static_cast<uint8_t>(*sps_id);
  return out;
}

struct VpsInfo {
  uint8_t vps_id{0};
  Ptl     ptl{};
};

/// Parses HEVC VPS NAL unit (including 2-byte NAL header) for `vps_id` and fallback PTL.
[[nodiscard]] inline std::optional<VpsInfo> parse_vps_info(std::span<const uint8_t> nal) noexcept {
  if (nal.size() < 18U) return std::nullopt;
  const NalHeader hdr = parse_nal_header(nal[0], nal[1]);
  if (!hdr.ok || hdr.type != 32U) return std::nullopt;

  std::array<uint8_t, 256> rbsp{};
  const size_t             n = strip_emulation_prevention(nal, 2U, rbsp.data(), rbsp.size());
  if (n < 16U) return std::nullopt;

  BitReader br(rbsp.data(), n);
  auto      vps_id     = br.read_bits(4);
  auto      base_flags = br.read_bits(2);
  auto      max_layers = br.read_bits(6);
  auto      max_sub_m1 = br.read_bits(3);
  auto      temp_nest  = br.read_bits(1);
  auto      reserved16 = br.read_bits(16);
  if (!vps_id || !base_flags || !max_layers || !max_sub_m1 || !temp_nest || !reserved16 ||
      *max_sub_m1 > 6U) {
    return std::nullopt;
  }

  VpsInfo info{};
  info.vps_id             = static_cast<uint8_t>(*vps_id);
  info.ptl.max_sub_layers = static_cast<uint8_t>(*max_sub_m1 + 1U);
  if (!detail::parse_profile_tier_level_bits(br, static_cast<uint8_t>(*max_sub_m1), info.ptl)) {
    return std::nullopt;
  }
  return info;
}

/// Extracts `vps_video_parameter_set_id` `u(4)` even on minimal VPS payloads.
[[nodiscard]] inline std::optional<uint8_t> parse_vps_id(std::span<const uint8_t> nal) noexcept {
  if (nal.size() < 3U) return std::nullopt;
  const NalHeader hdr = parse_nal_header(nal[0], nal[1]);
  if (!hdr.ok || hdr.type != 32U) return std::nullopt;
  return static_cast<uint8_t>((nal[2] >> 4U) & 0x0FU);
}

/// Extracts `pps_pic_parameter_set_id` `ue(v)` (`0..63`) from HEVC PPS NAL unit.
[[nodiscard]] inline std::optional<uint8_t> parse_pps_id(std::span<const uint8_t> nal) noexcept {
  if (nal.size() < 3U) return std::nullopt;
  const NalHeader hdr = parse_nal_header(nal[0], nal[1]);
  if (!hdr.ok || hdr.type != 34U) return std::nullopt;

  std::array<uint8_t, 32> rbsp{};
  const size_t            n = strip_emulation_prevention(nal, 2U, rbsp.data(), rbsp.size());
  if (n == 0U) return std::nullopt;
  BitReader br(rbsp.data(), n);
  auto      pps_id = br.read_ue();
  if (!pps_id || *pps_id >= 64U) return std::nullopt;
  return static_cast<uint8_t>(*pps_id);
}

} //< namespace rivet::media::hevc
