#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rivet::test {

/// Bitstream writer + RFC 7798 / ITU-T H.265 synthetic NAL and packetization builder.
class HevcSynth {
public:
  class BitWriter {
    std::vector<uint8_t> bytes_;
    uint8_t              cur_{0};
    uint8_t              bits_used_{0};

  public:
    void put_bit(bool b) {
      cur_ = static_cast<uint8_t>((cur_ << 1U) | (b ? 1U : 0U));
      if (++bits_used_ == 8U) {
        bytes_.push_back(cur_);
        cur_       = 0;
        bits_used_ = 0;
      }
    }

    void put_bits(uint64_t val, uint8_t count) {
      for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
        put_bit(((val >> static_cast<uint8_t>(i)) & 1ULL) != 0ULL);
      }
    }

    /// Exp-Golomb unsigned `ue(v)`
    void put_ue(uint32_t val) {
      const uint32_t code_num = val + 1U;
      uint8_t        bits     = 0;
      uint32_t       tmp      = code_num;
      while (tmp > 1U) {
        tmp >>= 1U;
        ++bits;
      }
      for (uint8_t i = 0; i < bits; ++i) {
        put_bit(false);
      }
      put_bits(code_num, static_cast<uint8_t>(bits + 1U));
    }

    [[nodiscard]] std::vector<uint8_t> finish_rbsp() {
      /// RBSP trailing stop bit + zero alignment
      put_bit(true);
      while (bits_used_ != 0U) {
        put_bit(false);
      }
      return bytes_;
    }
  };

  /// Inserts H.265 `0x00 00 03` emulation-prevention bytes for any `00 00 {00..03}` sequence.
  [[nodiscard]] static std::vector<uint8_t> rbsp_to_ebsp(const std::vector<uint8_t> &rbsp) {
    std::vector<uint8_t> out;
    out.reserve(rbsp.size() + 8);
    size_t zero_run = 0;
    for (uint8_t b : rbsp) {
      if (zero_run == 2U && b <= 0x03U) {
        out.push_back(0x03U);
        zero_run = 0;
      }
      out.push_back(b);
      if (b == 0x00U) {
        ++zero_run;
      } else {
        zero_run = 0;
      }
    }
    return out;
  }

  /// Builds a 2-byte H.265 NAL header: `[F(1) | Type(6) | LayerId(6) | TID(3)]`
  [[nodiscard]] static std::array<uint8_t, 2> make_nal_header(uint8_t nal_type,
      uint8_t                                                         layer_id = 0,
      uint8_t                                                         tid      = 1,
      bool forbidden_f                                                         = false) noexcept {
    const uint8_t b0 = static_cast<uint8_t>(
        (forbidden_f ? 0x80U : 0x00U) | ((nal_type & 0x3FU) << 1U) | ((layer_id >> 5U) & 0x01U));
    const uint8_t b1 = static_cast<uint8_t>(((layer_id & 0x1FU) << 3U) | (tid & 0x07U));
    return {b0, b1};
  }

  [[nodiscard]] static std::vector<uint8_t> make_nal(uint8_t nal_type,
      size_t                                                 payload_bytes = 16,
      uint8_t                                                tid           = 1,
      uint8_t                                                layer_id      = 0,
      bool                                                   forbidden_f   = false) {
    const auto           hdr = make_nal_header(nal_type, layer_id, tid, forbidden_f);
    std::vector<uint8_t> out;
    out.reserve(2 + payload_bytes);
    out.push_back(hdr[0]);
    out.push_back(hdr[1]);
    for (size_t i = 0; i < payload_bytes; ++i) {
      out.push_back(static_cast<uint8_t>(0x30U + (i & 0x7FU)));
    }
    return out;
  }

  /// Synthesizes a valid HEVC VPS (`NAL 32`) with configurable `vps_id`, `max_sub_layers`, and PTL.
  [[nodiscard]] static std::vector<uint8_t> make_vps(uint8_t vps_id         = 0,
      uint8_t                                                max_sub_layers = 1,
      uint8_t                                                profile_id     = 1,
      uint8_t                                                tier_flag      = 0,
      uint8_t                                                level_id       = 120) {
    BitWriter bw;
    bw.put_bits(vps_id & 0x0FU, 4);                                //< vps_video_parameter_set_id
    bw.put_bits(3U, 2);                                            //< vps_base_layer_internal/avail
    bw.put_bits(0U, 6);                                            //< vps_max_layers_minus1
    bw.put_bits((max_sub_layers > 0 ? max_sub_layers - 1 : 0), 3); //< vps_max_sub_layers_minus1
    bw.put_bit(max_sub_layers == 1);                               //< vps_temporal_id_nesting_flag
    bw.put_bits(0xFFFFU, 16);                                      //< vps_reserved_0xffff_16bits

    /// profile_tier_level(1, max_sub_layers - 1)
    bw.put_bits(0U, 2);                                   //< general_profile_space
    bw.put_bit(tier_flag != 0);                           //< general_tier_flag
    bw.put_bits(profile_id & 0x1FU, 5);                   //< general_profile_idc
    bw.put_bits(1ULL << (31 - (profile_id & 0x1FU)), 32); //< general_profile_compatibility_flags
    bw.put_bits(0xB00000000000ULL, 48);                   //< general_constraint_indicator_flags
    bw.put_bits(level_id, 8);                             //< general_level_idc

    /// Sub-layer profile/level flags if max_sub_layers > 1 (ITU-T H.265 §7.3.3)
    if (max_sub_layers > 1) {
      for (uint8_t i = 0; i < max_sub_layers - 1; ++i) {
        bw.put_bit(false); //< sub_layer_profile_present_flag[i]
        bw.put_bit(false); //< sub_layer_level_present_flag[i]
      }
      for (uint8_t i = max_sub_layers - 1; i < 8; ++i) {
        bw.put_bits(0U, 2); //< reserved_zero_2bits
      }
    }

    bw.put_bit(false);  //< vps_sub_layer_ordering_info_present_flag
    bw.put_ue(1U);      //< vps_max_dec_pic_buffering_minus1[max_sub_layers - 1]
    bw.put_ue(0U);      //< vps_max_num_reorder_pics[max_sub_layers - 1]
    bw.put_ue(0U);      //< vps_max_latency_increase_plus1[max_sub_layers - 1]
    bw.put_bits(0U, 6); //< vps_max_layer_id
    bw.put_ue(0U);      //< vps_num_layer_sets_minus1
    bw.put_bit(false);  //< vps_timing_info_present_flag
    bw.put_bit(false);  //< vps_extension_flag

    const auto           ebsp = rbsp_to_ebsp(bw.finish_rbsp());
    const auto           hdr  = make_nal_header(32, 0, 1);
    std::vector<uint8_t> nalu = {hdr[0], hdr[1]};
    nalu.insert(nalu.end(), ebsp.begin(), ebsp.end());
    return nalu;
  }

  /// Synthesizes a valid HEVC SPS (`NAL 33`) with configurable PTL and optional `00 00 03` EPB
  /// trigger!
  [[nodiscard]] static std::vector<uint8_t> make_sps(uint8_t sps_id         = 0,
      uint8_t                                                vps_id         = 0,
      uint8_t                                                max_sub_layers = 1,
      uint8_t                                                profile_space  = 0,
      uint8_t                                                tier_flag      = 0,
      uint8_t                                                profile_id     = 1,
      uint8_t                                                level_id       = 120,
      bool                                                   force_epb      = false) {
    BitWriter bw;
    bw.put_bits(vps_id & 0x0FU, 4);                                //< sps_video_parameter_set_id
    bw.put_bits((max_sub_layers > 0 ? max_sub_layers - 1 : 0), 3); //< sps_max_sub_layers_minus1
    bw.put_bit(max_sub_layers == 1);                               //< sps_temporal_id_nesting_flag

    /// profile_tier_level(1, max_sub_layers - 1)
    bw.put_bits(profile_space & 0x03U, 2);
    bw.put_bit(tier_flag != 0);
    bw.put_bits(profile_id & 0x1FU, 5);
    /// When `force_epb` is true, set compatibility/constraint bytes to `0x00 00 01` in RBSP so
    /// `rbsp_to_ebsp` injects `0x00 00 03 01` right before `general_level_idc`!
    if (force_epb) {
      bw.put_bits(0x00000100U, 32);
      bw.put_bits(0x000002B00000ULL, 48);
    } else {
      bw.put_bits(0x60000000U, 32);
      bw.put_bits(0xB00000000000ULL, 48);
    }
    bw.put_bits(level_id, 8);

    /// Sub-layer profile/level flags if max_sub_layers > 1
    if (max_sub_layers > 1) {
      for (uint8_t i = 0; i < max_sub_layers - 1; ++i) {
        bw.put_bit(false); //< sub_layer_profile_present_flag[i]
        bw.put_bit(false); //< sub_layer_level_present_flag[i]
      }
      for (uint8_t i = max_sub_layers - 1; i < 8; ++i) {
        bw.put_bits(0U, 2); //< reserved_zero_2bits
      }
    }

    bw.put_ue(sps_id); //< sps_seq_parameter_set_id
    bw.put_ue(1U);     //< chroma_format_idc = 1 (4:2:0)
    bw.put_ue(1920U);  //< pic_width_in_luma_samples
    bw.put_ue(1080U);  //< pic_height_in_luma_samples
    bw.put_bit(false); //< conformance_window_flag = 0
    bw.put_ue(0U);     //< bit_depth_luma_minus8
    bw.put_ue(0U);     //< bit_depth_chroma_minus8

    const auto           ebsp = rbsp_to_ebsp(bw.finish_rbsp());
    const auto           hdr  = make_nal_header(33, 0, 1);
    std::vector<uint8_t> nalu = {hdr[0], hdr[1]};
    nalu.insert(nalu.end(), ebsp.begin(), ebsp.end());
    return nalu;
  }

  /// Synthesizes a valid HEVC PPS (`NAL 34`) referencing `pps_id` and `sps_id`.
  [[nodiscard]] static std::vector<uint8_t>
  make_pps(uint8_t pps_id = 0, uint8_t sps_id = 0, uint8_t extra_tag = 0) {
    BitWriter bw;
    bw.put_ue(pps_id);
    bw.put_ue(sps_id);
    bw.put_bit(false);                 //< dependent_slice_segments_enabled_flag
    bw.put_bit(false);                 //< output_flag_present_flag
    bw.put_bits(extra_tag & 0x07U, 3); //< num_extra_slice_header_bits

    const auto           ebsp = rbsp_to_ebsp(bw.finish_rbsp());
    const auto           hdr  = make_nal_header(34, 0, 1);
    std::vector<uint8_t> nalu = {hdr[0], hdr[1]};
    nalu.insert(nalu.end(), ebsp.begin(), ebsp.end());
    return nalu;
  }

  [[nodiscard]] static std::vector<uint8_t> make_idr(size_t len = 64) {
    return make_nal(19 /* IDR_W_RADL */, len, 1);
  }
  [[nodiscard]] static std::vector<uint8_t> make_cra(size_t len = 64) {
    return make_nal(21 /* CRA_NUT */, len, 1);
  }
  [[nodiscard]] static std::vector<uint8_t> make_rasl_r(size_t len = 32) {
    return make_nal(9 /* RASL_R */, len, 1);
  }
  [[nodiscard]] static std::vector<uint8_t> make_radl_r(size_t len = 32) {
    return make_nal(7 /* RADL_R */, len, 1);
  }
  [[nodiscard]] static std::vector<uint8_t> make_trail_r(size_t len = 32, uint8_t tid = 1) {
    return make_nal(1 /* TRAIL_R */, len, tid);
  }
  [[nodiscard]] static std::vector<uint8_t> make_trail_n(size_t len = 24, uint8_t tid = 1) {
    return make_nal(0 /* TRAIL_N */, len, tid);
  }
  [[nodiscard]] static std::vector<uint8_t> make_tsa_r(size_t len = 32, uint8_t tid = 2) {
    return make_nal(3 /* TSA_R */, len, tid);
  }
  [[nodiscard]] static std::vector<uint8_t> make_stsa_r(size_t len = 32, uint8_t tid = 2) {
    return make_nal(5 /* STSA_R */, len, tid);
  }

  /// RFC 7798 §4.4.2 Aggregation Packet (`Type = 48`)
  [[nodiscard]] static std::vector<uint8_t> make_ap(const std::vector<std::vector<uint8_t>> &nalus,
      uint8_t  tid       = 1,
      bool     with_don  = false,
      uint16_t base_donl = 100) {
    const auto           hdr = make_nal_header(48, 0, tid);
    std::vector<uint8_t> out = {hdr[0], hdr[1]};
    for (size_t i = 0; i < nalus.size(); ++i) {
      if (with_don) {
        if (i == 0) {
          out.push_back(static_cast<uint8_t>((base_donl >> 8U) & 0xFFU));
          out.push_back(static_cast<uint8_t>(base_donl & 0xFFU));
        } else {
          out.push_back(0U); //< DOND = 0 -> DON increments by 1
        }
      }
      const uint16_t sz = static_cast<uint16_t>(nalus[i].size());
      out.push_back(static_cast<uint8_t>((sz >> 8U) & 0xFFU));
      out.push_back(static_cast<uint8_t>(sz & 0xFFU));
      out.insert(out.end(), nalus[i].begin(), nalus[i].end());
    }
    return out;
  }

  /// RFC 7798 §4.4.3 Fragmentation Unit (`Type = 49`)
  [[nodiscard]] static std::vector<std::vector<uint8_t>> fragment_fu(
      const std::vector<uint8_t> &nalu,
      size_t                      max_chunk = 1200,
      bool                        with_donl = false,
      uint16_t                    donl      = 0) {
    std::vector<std::vector<uint8_t>> frags;
    if (nalu.size() <= 2U || max_chunk == 0U) {
      return frags;
    }
    const uint8_t orig_type = static_cast<uint8_t>((nalu[0] >> 1U) & 0x3FU);
    const uint8_t layer_id =
        static_cast<uint8_t>(((nalu[0] & 0x01U) << 5U) | ((nalu[1] >> 3U) & 0x1FU));
    const uint8_t tid    = static_cast<uint8_t>(nalu[1] & 0x07U);
    const auto    fu_hdr = make_nal_header(49, layer_id, tid);

    const uint8_t *body     = nalu.data() + 2;
    const size_t   body_len = nalu.size() - 2;
    size_t         off      = 0;

    while (off < body_len) {
      const size_t  chunk    = std::min(max_chunk, body_len - off);
      const bool    is_start = (off == 0U);
      const bool    is_end   = (off + chunk == body_len);
      const uint8_t fuh      = static_cast<uint8_t>(
          (is_start ? 0x80U : 0x00U) | (is_end ? 0x40U : 0x00U) | (orig_type & 0x3FU));

      std::vector<uint8_t> pkt;
      pkt.reserve(3 + (with_donl ? 2 : 0) + chunk);
      pkt.push_back(fu_hdr[0]);
      pkt.push_back(fu_hdr[1]);
      pkt.push_back(fuh);
      if (with_donl) {
        pkt.push_back(static_cast<uint8_t>((donl >> 8U) & 0xFFU));
        pkt.push_back(static_cast<uint8_t>(donl & 0xFFU));
      }
      pkt.insert(pkt.end(), body + off, body + off + chunk);
      frags.push_back(std::move(pkt));
      off += chunk;
    }
    return frags;
  }

  /// RFC 7798 §4.4.4 PACI Packet (`Type = 50`) wrapping `inner_payload` (without its 2B NAL header)
  [[nodiscard]] static std::vector<uint8_t> wrap_paci(const std::vector<uint8_t> &inner_body,
      uint8_t                                                                     ctype,
      const std::vector<uint8_t>                                                 &phs   = {0xAA},
      uint8_t                                                                     tid   = 1,
      bool                                                                        a_bit = false) {
    const auto    hdr      = make_nal_header(50, 0, tid);
    const uint8_t phs_size = static_cast<uint8_t>(phs.size() & 0x1FU);
    const uint8_t p0       = static_cast<uint8_t>(
        (a_bit ? 0x80U : 0x00U) | ((ctype & 0x3FU) << 1U) | ((phs_size >> 4U) & 0x01U));
    const uint8_t        p1 = static_cast<uint8_t>((phs_size & 0x0FU) << 4U);
    std::vector<uint8_t> out;
    out.reserve(4 + phs_size + inner_body.size());
    out.push_back(hdr[0]);
    out.push_back(hdr[1]);
    out.push_back(p0);
    out.push_back(p1);
    out.insert(out.end(), phs.begin(), phs.begin() + phs_size);
    out.insert(out.end(), inner_body.begin(), inner_body.end());
    return out;
  }
};

} //< namespace rivet::test
