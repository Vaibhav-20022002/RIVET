#pragma once

#include <cstdint>
#include <vector>

namespace rivet::test {

/// Fluid builder for synthesizing RFC 3550 RTP packets
class RtpBuilder {
  uint8_t  version_{2};
  bool     padding_{false};
  bool     extension_{false};
  uint8_t  csrc_count_{0};
  bool     marker_{false};
  uint8_t  payload_type_{96};
  uint16_t seq_{1};
  uint32_t timestamp_{90000};
  uint32_t ssrc_{0x12345678};

  std::vector<uint32_t> csrcs_;
  uint16_t              ext_profile_{0};
  std::vector<uint32_t> ext_words_;
  std::vector<uint8_t>  payload_;
  uint8_t               padding_len_{0};

public:
  RtpBuilder() = default;

  RtpBuilder &version(uint8_t v) {
    version_ = v;
    return *this;
  }
  RtpBuilder &marker(bool m) {
    marker_ = m;
    return *this;
  }
  RtpBuilder &payload_type(uint8_t pt) {
    payload_type_ = pt;
    return *this;
  }
  RtpBuilder &seq(uint16_t s) {
    seq_ = s;
    return *this;
  }
  RtpBuilder &timestamp(uint32_t ts) {
    timestamp_ = ts;
    return *this;
  }
  RtpBuilder &ssrc(uint32_t s) {
    ssrc_ = s;
    return *this;
  }

  RtpBuilder &add_csrc(uint32_t c) {
    csrcs_.push_back(c);
    csrc_count_ = static_cast<uint8_t>(csrcs_.size());
    return *this;
  }

  RtpBuilder &extension(uint16_t profile, const std::vector<uint32_t> &words) {
    extension_   = true;
    ext_profile_ = profile;
    ext_words_   = words;
    return *this;
  }

  RtpBuilder &payload(const std::vector<uint8_t> &p) {
    payload_ = p;
    return *this;
  }
  RtpBuilder &payload(const uint8_t *data, size_t len) {
    payload_.assign(data, data + len);
    return *this;
  }

  RtpBuilder &padding(uint8_t pad_bytes) {
    padding_     = (pad_bytes > 0);
    padding_len_ = pad_bytes;
    return *this;
  }

  [[nodiscard]] std::vector<uint8_t> build() const {
    std::vector<uint8_t> pkt;
    pkt.reserve(12 + csrcs_.size() * 4 + (extension_ ? 4 + ext_words_.size() * 4 : 0) +
        payload_.size() + padding_len_);

    uint8_t byte0 =
        (version_ << 6) | (padding_ ? 0x20 : 0) | (extension_ ? 0x10 : 0) | (csrc_count_ & 0x0F);
    uint8_t byte1 = (marker_ ? 0x80 : 0) | (payload_type_ & 0x7F);

    pkt.push_back(byte0);
    pkt.push_back(byte1);
    pkt.push_back(static_cast<uint8_t>((seq_ >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(seq_ & 0xFF));

    pkt.push_back(static_cast<uint8_t>((timestamp_ >> 24) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((timestamp_ >> 16) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((timestamp_ >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(timestamp_ & 0xFF));

    pkt.push_back(static_cast<uint8_t>((ssrc_ >> 24) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((ssrc_ >> 16) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((ssrc_ >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(ssrc_ & 0xFF));

    for (uint32_t c : csrcs_) {
      pkt.push_back(static_cast<uint8_t>((c >> 24) & 0xFF));
      pkt.push_back(static_cast<uint8_t>((c >> 16) & 0xFF));
      pkt.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
      pkt.push_back(static_cast<uint8_t>(c & 0xFF));
    }

    if (extension_) {
      pkt.push_back(static_cast<uint8_t>((ext_profile_ >> 8) & 0xFF));
      pkt.push_back(static_cast<uint8_t>(ext_profile_ & 0xFF));
      uint16_t num_words = static_cast<uint16_t>(ext_words_.size());
      pkt.push_back(static_cast<uint8_t>((num_words >> 8) & 0xFF));
      pkt.push_back(static_cast<uint8_t>(num_words & 0xFF));
      for (uint32_t w : ext_words_) {
        pkt.push_back(static_cast<uint8_t>((w >> 24) & 0xFF));
        pkt.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
        pkt.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
        pkt.push_back(static_cast<uint8_t>(w & 0xFF));
      }
    }

    pkt.insert(pkt.end(), payload_.begin(), payload_.end());

    if (padding_ && padding_len_ > 0) {
      for (uint8_t i = 1; i < padding_len_; ++i) {
        pkt.push_back(0);
      }
      pkt.push_back(padding_len_);
    }

    return pkt;
  }
};

} //< namespace rivet::test
