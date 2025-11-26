#include "manet/utils/hexdump.hpp"

namespace manet::utils
{

/** simple %-encoding and hexformat unreadable chars */
std::string readable_ascii(std::string_view input_str)
{
  std::string str = "";
  str.reserve(input_str.size());

  for (int i = 0; i < input_str.size(); i++)
  {
    auto c = input_str[i];

    if (c < ' ' || '~' < c)
    {
      static const char *hex = "0123456789abcdef";
      str.append("%");
      str.push_back(hex[c >> 4]);
      str.push_back(hex[c & 0x0F]);
    }
    else if (c == '%')
    {
      str.append("%%");
    }
    else
    {
      str.push_back(static_cast<char>(c));
    }
  }

  return str;
}

std::string hexdump(std::span<const std::byte> s, std::size_t base)
{
  std::string out;
  if (s.empty())
    return out;
  out.reserve(s.size() * 4); // rough

  auto u8 = [](std::byte b) { return std::to_integer<unsigned char>(b); };
  const std::size_t N = s.size();
  for (std::size_t i = 0; i < N; i += 16)
  {
    const std::size_t n = (N - i < 16) ? (N - i) : 16;

    // offset
    out += std::format("{:08x}  ", static_cast<unsigned>(base + i));

    // hex bytes
    for (std::size_t j = 0; j < 16; ++j)
    {
      if (j < n)
        out += std::format("{:02x} ", u8(s[i + j]));
      else
        out += "   ";
      if (j == 7)
        out += " ";
    }

    // ascii gutter
    out += " |";
    for (std::size_t j = 0; j < n; ++j)
    {
      unsigned c = u8(s[i + j]);
      out += (c >= 32 && c <= 126) ? static_cast<char>(c) : '.';
    }
    out += "|\n";
  }
  return out;
}

} // namespace manet::utils
