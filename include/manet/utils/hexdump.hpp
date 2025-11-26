#pragma once
#include <format>

namespace manet::utils
{

std::string readable_ascii(std::string_view input_str);
std::string hexdump(std::span<const std::byte> s, std::size_t base = 0);

} // namespace manet::utils
