#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <span>
#include <string>
#include <vector>

#include <manet/protocol/websocket_frame.hpp>

using namespace manet::protocol::websocket;

// helper to make readable byte buffers
static std::vector<std::byte> make_bytes(std::initializer_list<uint8_t> bytes)
{
  std::vector<std::byte> v;
  v.reserve(bytes.size());
  for (auto b : bytes)
    v.push_back(static_cast<std::byte>(b));
  return v;
}

static std::string to_string(std::span<const std::byte> s)
{
  std::string out;
  out.reserve(s.size());
  for (auto b : s)
    out.push_back(static_cast<char>(b));
  return out;
}

TEST_CASE("parse_frame: simple unmasked text frame, short length")
{
  // FIN=1, RSV=0, opcode=text(0x1)
  // MASK=0, payload_len=5 ("Hello")
  auto buf = make_bytes(
    {0x81, // 1000 0001 -> FIN + text
     0x05, // 0000 0101 -> no mask, length=5
     'H', 'e', 'l', 'l', 'o'}
  );

  parse_output out{};
  auto status =
    parse_frame(std::span<const std::byte>(buf.data(), buf.size()), out);

  REQUIRE(status == parse_status::ok);

  CHECK(out.consumed == buf.size());
  CHECK(out.frame.op == OpCode::text);
  CHECK(out.frame.fin == true);
  CHECK(out.frame.payload_len == 5);
  CHECK(out.frame.payload.size() == 5);
  CHECK(to_string(out.frame.payload) == "Hello");
}

TEST_CASE("parse_frame: unmasked binary frame with 16-bit extended length")
{
  // payload length = 126 -> encoded in 16-bit extended form
  const std::size_t payload_len = 126;

  std::vector<std::byte> buf;
  buf.reserve(2 + 2 + payload_len);

  // FIN=1, opcode=binary(0x2)
  buf.push_back(static_cast<std::byte>(0x82));
  // MASK=0, 7-bit length=126 => 0x7E
  buf.push_back(static_cast<std::byte>(0x7E));
  // extended length: 0x00 0x7E (big-endian)
  buf.push_back(static_cast<std::byte>(0x00));
  buf.push_back(static_cast<std::byte>(0x7E));

  // payload: 126 bytes of 'x'
  for (std::size_t i = 0; i < payload_len; ++i)
    buf.push_back(static_cast<std::byte>('x'));

  parse_output out{};
  auto status =
    parse_frame(std::span<const std::byte>(buf.data(), buf.size()), out);

  REQUIRE(status == parse_status::ok);

  CHECK(out.frame.op == OpCode::binary);
  CHECK(out.frame.fin == true);
  CHECK(out.frame.payload_len == payload_len);
  CHECK(out.frame.payload.size() == payload_len);
  CHECK(out.consumed == buf.size());
}

TEST_CASE("parse_frame: unmasked binary frame with 64-bit extended length")
{
  // Use a 64-bit length just over 65535 to force 64-bit encoding
  const uint64_t payload_len = 65536; // 64 KiB

  std::vector<std::byte> buf;
  buf.reserve(2 + 8 + payload_len);

  // FIN=1, opcode=binary(0x2)
  buf.push_back(static_cast<std::byte>(0x82));
  // MASK=0, 7-bit length=127 => 64-bit extended length
  buf.push_back(static_cast<std::byte>(0x7F));

  // 64-bit big-endian length
  uint64_t len = payload_len;
  for (int shift = 56; shift >= 0; shift -= 8)
  {
    buf.push_back(static_cast<std::byte>((len >> shift) & 0xFF));
  }

  // payload: 64 KiB of 'y'
  for (uint64_t i = 0; i < payload_len; ++i)
    buf.push_back(static_cast<std::byte>('y'));

  parse_output out{};
  auto status =
    parse_frame(std::span<const std::byte>(buf.data(), buf.size()), out);

  REQUIRE(status == parse_status::ok);

  CHECK(out.frame.op == OpCode::binary);
  CHECK(out.frame.fin == true);
  CHECK(out.frame.payload_len == payload_len);
  CHECK(out.frame.payload.size() == payload_len);
  CHECK(out.consumed == buf.size());
}

TEST_CASE("parse_frame: masked frame from server is rejected")
{
  // FIN=1, opcode=text(0x1)
  // MASK=1, payload_len=3, "hey"
  auto buf = make_bytes(
    {0x81,                   // FIN + text
     0x83,                   // 1000 0011 -> MASK=1, len=3
     0x01, 0x02, 0x03, 0x04, // masking key (ignored if rejected early)
     'h', 'e', 'y'}
  );

  parse_output out{};
  auto status =
    parse_frame(std::span<const std::byte>(buf.data(), buf.size()), out);

  CHECK(status == parse_status::masked_server);
}

TEST_CASE("parse_frame: RSV bits set is rejected")
{
  // FIN=1, RSV1=1, opcode=text(0x1)
  // First byte: 1110 0001 -> 0xE1
  auto buf = make_bytes(
    {0xE1, // FIN + RSV1 + text
     0x03, // MASK=0, len=3
     'b', 'a', 'd'}
  );

  parse_output out{};
  auto status =
    parse_frame(std::span<const std::byte>(buf.data(), buf.size()), out);

  CHECK(status == parse_status::bad_reserved);
}

TEST_CASE("parse_frame: need_more for partial header and payload")
{
  SUBCASE("only first header byte present")
  {
    auto buf = make_bytes({
      0x81 // FIN + text, but no length byte
    });

    parse_output out{};
    auto status =
      parse_frame(std::span<const std::byte>(buf.data(), buf.size()), out);

    CHECK(status == parse_status::need_more);
  }

  SUBCASE("only 3 header bytes present (len=126)")
  {
    auto buf = make_bytes({0x81, 0x7e, 'A'});

    parse_output out{};
    auto status =
      parse_frame(std::span<const std::byte>(buf.data(), buf.size()), out);

    CHECK(status == parse_status::need_more);
  }

  SUBCASE("only 3 header bytes present (len=127)")
  {
    auto buf = make_bytes({0x81, 0x7f, 'a'});

    parse_output out{};
    auto status =
      parse_frame(std::span<const std::byte>(buf.data(), buf.size()), out);

    CHECK(status == parse_status::need_more);
  }

  SUBCASE("header complete but payload truncated")
  {
    // Declared payload len = 5, but only 2 payload bytes present
    auto buf = make_bytes(
      {0x81, // FIN + text
       0x05, // len=5
       'H', 'i'}
    );

    parse_output out{};
    auto status =
      parse_frame(std::span<const std::byte>(buf.data(), buf.size()), out);

    CHECK(status == parse_status::need_more);
  }
}
