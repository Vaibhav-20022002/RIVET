#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::media::base64 {

inline constexpr char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

consteval std::array<int8_t, 256> make_decode_table() {
  std::array<int8_t, 256> t{};
  for (auto &v : t) v = -1;
  for (int i = 0; i < 26; ++i) {
    t[static_cast<size_t>('A' + i)] = static_cast<int8_t>(i);
    t[static_cast<size_t>('a' + i)] = static_cast<int8_t>(26 + i);
  }
  for (int i = 0; i < 10; ++i) {
    t[static_cast<size_t>('0' + i)] = static_cast<int8_t>(52 + i);
  }
  t[static_cast<size_t>('+')] = 62;
  t[static_cast<size_t>('/')] = 63;
  t[static_cast<size_t>('=')] = -2; //< Padding sentinel
  return t;
}

inline constexpr auto kDecodeTable = make_decode_table();

[[nodiscard]] inline std::string encode(std::span<const uint8_t> data) {
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);

  size_t i = 0;
  while (i + 3 <= data.size()) {
    const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16U) |
        (static_cast<uint32_t>(data[i + 1]) << 8U) | static_cast<uint32_t>(data[i + 2]);
    out.push_back(kEncodeTable[(triple >> 18U) & 0x3FU]);
    out.push_back(kEncodeTable[(triple >> 12U) & 0x3FU]);
    out.push_back(kEncodeTable[(triple >> 6U) & 0x3FU]);
    out.push_back(kEncodeTable[triple & 0x3FU]);
    i += 3;
  }

  const size_t rem = data.size() - i;
  if (rem == 1) {
    const uint32_t triple = static_cast<uint32_t>(data[i]) << 16U;
    out.push_back(kEncodeTable[(triple >> 18U) & 0x3FU]);
    out.push_back(kEncodeTable[(triple >> 12U) & 0x3FU]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const uint32_t triple =
        (static_cast<uint32_t>(data[i]) << 16U) | (static_cast<uint32_t>(data[i + 1]) << 8U);
    out.push_back(kEncodeTable[(triple >> 18U) & 0x3FU]);
    out.push_back(kEncodeTable[(triple >> 12U) & 0x3FU]);
    out.push_back(kEncodeTable[(triple >> 6U) & 0x3FU]);
    out.push_back('=');
  }
  return out;
}

/// Strict RFC 4648 Base64 decoder rejecting invalid alphabet characters or malformed padding.
[[nodiscard]] inline std::optional<std::vector<uint8_t>> decode(std::string_view input) {
  while (!input.empty() && (input.front() == ' ' || input.front() == '\t')) input.remove_prefix(1);
  while (!input.empty() && (input.back() == ' ' || input.back() == '\t')) input.remove_suffix(1);

  if (input.empty()) {
    return std::vector<uint8_t>{};
  }
  if (input.size() % 4 != 0) {
    return std::nullopt;
  }

  std::vector<uint8_t> out;
  out.reserve((input.size() / 4) * 3);

  for (size_t i = 0; i < input.size(); i += 4) {
    int8_t v0 = kDecodeTable[static_cast<uint8_t>(input[i])];
    int8_t v1 = kDecodeTable[static_cast<uint8_t>(input[i + 1])];
    int8_t v2 = kDecodeTable[static_cast<uint8_t>(input[i + 2])];
    int8_t v3 = kDecodeTable[static_cast<uint8_t>(input[i + 3])];

    if (v0 < 0 || v1 < 0 || v2 == -1 || v3 == -1) {
      return std::nullopt;
    }
    /// Padding '=' is only permitted in the final 4-char quantum
    if ((v2 == -2 || v3 == -2) && (i + 4 != input.size())) {
      return std::nullopt;
    }
    if (v2 == -2 && v3 != -2) {
      return std::nullopt;
    }

    const uint32_t b0     = static_cast<uint32_t>(v0);
    const uint32_t b1     = static_cast<uint32_t>(v1);
    const uint32_t b2     = (v2 >= 0) ? static_cast<uint32_t>(v2) : 0U;
    const uint32_t b3     = (v3 >= 0) ? static_cast<uint32_t>(v3) : 0U;
    const uint32_t triple = (b0 << 18U) | (b1 << 12U) | (b2 << 6U) | b3;

    out.push_back(static_cast<uint8_t>((triple >> 16U) & 0xFFU));
    if (v2 != -2) {
      out.push_back(static_cast<uint8_t>((triple >> 8U) & 0xFFU));
    }
    if (v3 != -2) {
      out.push_back(static_cast<uint8_t>(triple & 0xFFU));
    }
  }

  return out;
}

} //< namespace rivet::media::base64
