#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h>
#elif defined(__linux__)
#include <random>
#include <sys/random.h>
#else
#include <random>
#endif

namespace rivet::proto::rtsp {

/// Generates 64 bits of cryptographic randomness using OS entropy (`arc4random_buf` / `getrandom`).
[[nodiscard]] inline uint64_t csprng_u64() noexcept {
  uint64_t val = 0;
#if defined(__APPLE__) || defined(__FreeBSD__)
  arc4random_buf(&val, sizeof(val));
#elif defined(__linux__)
  if (::getrandom(&val, sizeof(val), 0) != static_cast<ssize_t>(sizeof(val))) {
    std::random_device rd;
    val = (static_cast<uint64_t>(rd()) << 32U) ^ static_cast<uint64_t>(rd());
  }
#else
  std::random_device rd;
  val = (static_cast<uint64_t>(rd()) << 32U) ^ static_cast<uint64_t>(rd());
#endif
  if (val == 0ULL) val = 0x9E3779B97F4A7C15ULL;
  return val;
}

[[nodiscard]] inline uint32_t csprng_non_zero_u32() noexcept {
  const uint32_t v = static_cast<uint32_t>(csprng_u64() & 0xFFFFFFFFULL);
  return (v != 0U) ? v : 0x5A17C3E9U;
}

[[nodiscard]] inline std::string generate_random_hex_id() {
  const uint64_t v = csprng_u64();
  char           buf[17]{};
  std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
  return std::string(buf, 16);
}

/// Self-contained RFC 1321 MD5 implementation solely for RFC 2617 / RFC 7826 RTSP Digest Auth
/// interoperability with FFmpeg, OBS, GStreamer, and VLC (zero external OpenSSL dependency).
namespace md5_detail {

[[nodiscard]] constexpr uint32_t rotl32(uint32_t x, uint32_t c) noexcept {
  return (x << c) | (x >> (32U - c));
}

inline constexpr std::array<uint32_t, 64> kShift = {7,
    12,
    17,
    22,
    7,
    12,
    17,
    22,
    7,
    12,
    17,
    22,
    7,
    12,
    17,
    22,
    5,
    9,
    14,
    20,
    5,
    9,
    14,
    20,
    5,
    9,
    14,
    20,
    5,
    9,
    14,
    20,
    4,
    11,
    16,
    23,
    4,
    11,
    16,
    23,
    4,
    11,
    16,
    23,
    4,
    11,
    16,
    23,
    6,
    10,
    15,
    21,
    6,
    10,
    15,
    21,
    6,
    10,
    15,
    21,
    6,
    10,
    15,
    21};

inline constexpr std::array<uint32_t, 64> kTable = {0xd76aa478U,
    0xe8c7b756U,
    0x242070dbU,
    0xc1bdceeeU,
    0xf57c0fafU,
    0x4787c62aU,
    0xa8304613U,
    0xfd469501U,
    0x698098d8U,
    0x8b44f7afU,
    0xffff5bb1U,
    0x895cd7beU,
    0x6b901122U,
    0xfd987193U,
    0xa679438eU,
    0x49b40821U,
    0xf61e2562U,
    0xc040b340U,
    0x265e5a51U,
    0xe9b6c7aaU,
    0xd62f105dU,
    0x02441453U,
    0xd8a1e681U,
    0xe7d3fbc8U,
    0x21e1cde6U,
    0xc33707d6U,
    0xf4d50d87U,
    0x455a14edU,
    0xa9e3e905U,
    0xfcefa3f8U,
    0x676f02d9U,
    0x8d2a4c8aU,
    0xfffa3942U,
    0x8771f681U,
    0x6d9d6122U,
    0xfde5380cU,
    0xa4beea44U,
    0x4bdecfa9U,
    0xf6bb4b60U,
    0xbebfbc70U,
    0x289b7ec6U,
    0xeaa127faU,
    0xd4ef3085U,
    0x04881d05U,
    0xd9d4d039U,
    0xe6db99e5U,
    0x1fa27cf8U,
    0xc4ac5665U,
    0xf4292244U,
    0x432aff97U,
    0xab9423a7U,
    0xfc93a039U,
    0x655b59c3U,
    0x8f0ccc92U,
    0xffeff47dU,
    0x85845dd1U,
    0x6fa87e4fU,
    0xfe2ce6e0U,
    0xa3014314U,
    0x4e0811a1U,
    0xf7537e82U,
    0xbd3af235U,
    0x2ad7d2bbU,
    0xeb86d391U};

[[nodiscard]] inline std::string md5_hex(std::string_view input) {
  uint32_t a0 = 0x67452301U;
  uint32_t b0 = 0xefcdab89U;
  uint32_t c0 = 0x98badcfeU;
  uint32_t d0 = 0x10325476U;

  const uint64_t bit_len    = static_cast<uint64_t>(input.size()) * 8ULL;
  size_t         padded_len = input.size() + 1U;
  while ((padded_len % 64U) != 56U) ++padded_len;
  padded_len += 8U;

  std::string msg(padded_len, '\0');
  if (!input.empty()) {
    std::memcpy(msg.data(), input.data(), input.size());
  }
  msg[input.size()] = static_cast<char>(0x80U);
  for (size_t i = 0; i < 8U; ++i) {
    msg[padded_len - 8U + i] = static_cast<char>((bit_len >> (i * 8U)) & 0xFFULL);
  }

  for (size_t chunk = 0; chunk < padded_len; chunk += 64U) {
    uint32_t M[16]{};
    for (size_t i = 0; i < 16U; ++i) {
      const auto *p = reinterpret_cast<const uint8_t *>(msg.data() + chunk + i * 4U);
      M[i]          = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
          (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
    }

    uint32_t A = a0, B = b0, C = c0, D = d0;
    for (uint32_t i = 0; i < 64U; ++i) {
      uint32_t F = 0, g = 0;
      if (i < 16U) {
        F = (B & C) | ((~B) & D);
        g = i;
      } else if (i < 32U) {
        F = (D & B) | ((~D) & C);
        g = (5U * i + 1U) % 16U;
      } else if (i < 48U) {
        F = B ^ C ^ D;
        g = (3U * i + 5U) % 16U;
      } else {
        F = C ^ (B | (~D));
        g = (7U * i) % 16U;
      }
      F = F + A + kTable[i] + M[g];
      A = D;
      D = C;
      C = B;
      B = B + rotl32(F, kShift[i]);
    }
    a0 += A;
    b0 += B;
    c0 += C;
    d0 += D;
  }

  uint8_t digest[16]{};
  auto    store_le = [&](size_t off, uint32_t v) {
    digest[off + 0] = static_cast<uint8_t>(v & 0xFFU);
    digest[off + 1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    digest[off + 2] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    digest[off + 3] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
  };
  store_le(0, a0);
  store_le(4, b0);
  store_le(8, c0);
  store_le(12, d0);

  char hex[33]{};
  for (size_t i = 0; i < 16U; ++i) {
    std::snprintf(hex + i * 2U, 3, "%02x", digest[i]);
  }
  return std::string(hex, 32);
}

[[nodiscard]] inline std::string_view extract_auth_param(std::string_view auth_hdr,
    std::string_view                                                      key) noexcept {
  size_t pos = 0;
  while (pos < auth_hdr.size()) {
    const size_t found = auth_hdr.find(key, pos);
    if (found == std::string_view::npos) return {};
    const bool   boundary_ok = (found == 0U || auth_hdr[found - 1U] == ' ' ||
        auth_hdr[found - 1U] == ',' || auth_hdr[found - 1U] == '\t');
    const size_t after       = found + key.size();
    if (boundary_ok && after < auth_hdr.size() && auth_hdr[after] == '=') {
      size_t vstart = after + 1U;
      if (vstart < auth_hdr.size() && auth_hdr[vstart] == '"') {
        ++vstart;
        const size_t vend = auth_hdr.find('"', vstart);
        if (vend == std::string_view::npos) return {};
        return auth_hdr.substr(vstart, vend - vstart);
      }
      const size_t vend = auth_hdr.find_first_of(", \r\n", vstart);
      return (vend == std::string_view::npos) ? auth_hdr.substr(vstart)
                                              : auth_hdr.substr(vstart, vend - vstart);
    }
    pos = after;
  }
  return {};
}

[[nodiscard]] inline bool constant_time_hex_eq(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
    char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
    diff |= static_cast<uint8_t>(ca ^ cb);
  }
  return diff == 0U;
}

} //< namespace md5_detail

/// Validates an RFC 2617 `Authorization: Digest ...` header value against expected credentials.
[[nodiscard]] inline bool verify_rtsp_digest_auth(std::string_view auth_header_value,
    std::string_view                                               method,
    std::string_view                                               expected_realm,
    std::string_view                                               expected_nonce,
    std::string_view                                               expected_user,
    std::string_view                                               expected_password) {
  if (expected_user.empty()) return true;
  while (!auth_header_value.empty() &&
      (auth_header_value.front() == ' ' || auth_header_value.front() == '\t')) {
    auth_header_value.remove_prefix(1);
  }
  if (auth_header_value.size() < 7U) return false;
  /// Case-insensitive "Digest " prefix check
  std::string_view scheme = auth_header_value.substr(0, 6U);
  for (size_t i = 0; i < 6U; ++i) {
    char c1 =
        (scheme[i] >= 'A' && scheme[i] <= 'Z') ? static_cast<char>(scheme[i] + 32) : scheme[i];
    if (c1 != "digest"[i]) return false;
  }

  const auto username = md5_detail::extract_auth_param(auth_header_value, "username");
  const auto realm    = md5_detail::extract_auth_param(auth_header_value, "realm");
  const auto nonce    = md5_detail::extract_auth_param(auth_header_value, "nonce");
  const auto uri      = md5_detail::extract_auth_param(auth_header_value, "uri");
  const auto response = md5_detail::extract_auth_param(auth_header_value, "response");
  const auto qop      = md5_detail::extract_auth_param(auth_header_value, "qop");
  const auto nc       = md5_detail::extract_auth_param(auth_header_value, "nc");
  const auto cnonce   = md5_detail::extract_auth_param(auth_header_value, "cnonce");

  if (username != expected_user || realm != expected_realm || nonce != expected_nonce ||
      uri.empty() || response.size() != 32U) {
    return false;
  }

  const std::string ha1 = md5_detail::md5_hex(std::string(expected_user) + ":" +
      std::string(expected_realm) + ":" + std::string(expected_password));
  const std::string ha2 = md5_detail::md5_hex(std::string(method) + ":" + std::string(uri));

  std::string expected_resp;
  if (!qop.empty()) {
    expected_resp = md5_detail::md5_hex(ha1 + ":" + std::string(expected_nonce) + ":" +
        std::string(nc) + ":" + std::string(cnonce) + ":" + std::string(qop) + ":" + ha2);
  } else {
    expected_resp = md5_detail::md5_hex(ha1 + ":" + std::string(expected_nonce) + ":" + ha2);
  }

  return md5_detail::constant_time_hex_eq(response, expected_resp);
}

} //< namespace rivet::proto::rtsp
