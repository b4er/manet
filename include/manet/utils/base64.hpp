#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace manet::utils
{

namespace
{
static constexpr std::string_view table =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

template <std::size_t M, std::size_t N>
constexpr std::string_view base64_encode(
  const std::array<std::byte, M> &input, std::array<char, N> &out
) noexcept
{
  static_assert(N == 4 * ((M + 2) / 3));

  const uint8_t *src = reinterpret_cast<const uint8_t *>(input.data());
  char *pos = out.data();
  const uint8_t *end = src + M;

  while (end - src >= 3)
  {
    *pos++ = table[src[0] >> 2];
    *pos++ = table[((src[0] & 0x03) << 4) | (src[1] >> 4)];
    *pos++ = table[((src[1] & 0x0f) << 2) | (src[2] >> 6)];
    *pos++ = table[src[2] & 0x3f];
    src += 3;
  }

  if (auto rem = end - src; rem)
  {
    *pos++ = table[src[0] >> 2];
    if (rem == 1)
    {
      *pos++ = table[(src[0] & 0x03) << 4];
      *pos++ = '=';
    }
    else
    {
      *pos++ = table[((src[0] & 0x03) << 4) | (src[1] >> 4)];
      *pos++ = table[(src[1] & 0x0f) << 2];
    }
    *pos++ = '=';
  }

  return {out.data(), static_cast<std::size_t>(pos - out.data())};
}

} // namespace manet::utils
