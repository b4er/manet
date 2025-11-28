#pragma once

#include <cstdint>
#include <span>

namespace manet::protocol::websocket::detail
{

enum class OpCode : uint8_t
{
  cont = 0x0,
  text = 0x1,
  binary = 0x2,
  close = 0x8,
  ping = 0x9,
  pong = 0xA,
};

enum class CloseCode : uint16_t
{
  normal = 1000,
  going_away = 1001,
  protocol_error = 1002,
  unsupported = 1003,
  invalid_payload = 1007,
  policy_violation = 1008,
  msg_too_big = 1009,
  mandatory_ext = 1010,
  internal_error = 1011,
};

enum class parse_status : uint8_t
{
  ok,
  need_more,
  masked_server, // server -> client must not be masked
  bad_reserved,  // RSV bits set
};

// non-owning view
struct frame_view
{
  OpCode op;
  bool fin;

  std::span<const std::byte> payload;
  uint64_t payload_len;
};

struct parse_output
{
  frame_view frame;
  std::size_t consumed; // header + payload bytes consumed from input buffer
};

// inlinable frame utils (detail)
constexpr bool fin_bit(uint8_t b0) noexcept { return (b0 & 0x80) != 0; }
constexpr uint8_t rsv(uint8_t b0) noexcept { return (b0 >> 4) & 0x7; }
constexpr OpCode to_op(uint8_t b0) noexcept
{
  return static_cast<OpCode>(b0 & 0x0F);
}
constexpr bool masked(uint8_t b1) noexcept { return (b1 & 0x80) != 0; }
constexpr uint8_t len7(uint8_t b1) noexcept { return uint8_t(b1 & 0x7F); }

// single pass, non-throwing parse view
inline parse_status
parse_frame(std::span<const std::byte> in, parse_output &out) noexcept
{
  if (in.size() < 2)
  {
    return parse_status::need_more;
  }

  const uint8_t *buf = reinterpret_cast<const uint8_t *>(in.data());
  const uint8_t b0 = buf[0];
  const uint8_t b1 = buf[1];

  if (rsv(b0) != 0)
  {
    return parse_status::bad_reserved;
  }

  if (masked(b1))
  {
    // RFC: server->client unmasked
    return parse_status::masked_server;
  }

  uint64_t len = len7(b1);
  std::size_t hdr = 2;

  if (len == 126)
  {
    if (in.size() < 4)
    {
      return parse_status::need_more;
    }
    len = (uint64_t(buf[2]) << 8) | buf[3];
    hdr += 2;
  }
  else if (len == 127)
  {
    if (in.size() < 10)
    {
      return parse_status::need_more;
    }
    uint64_t x = 0;
    for (int i = 0; i < 8; i++)
    {
      x = (x << 8) | buf[2 + i];
    }
    len = x;
    hdr += 8;
  }

  if (in.size() < hdr + len)
  {
    return parse_status::need_more;
  }

  // set outputs
  out.frame.op = to_op(b0);
  out.frame.fin = fin_bit(b0);

  out.frame.payload_len = len;
  out.frame.payload = std::span<const std::byte>(in.subspan(hdr, size_t(len)));

  out.consumed = hdr + size_t(len);

  return parse_status::ok;
}

} // namespace manet::protocol::websocket::detail
