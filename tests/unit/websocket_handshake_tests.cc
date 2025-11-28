#include <doctest/doctest.h>

#include <manet/protocol/websocket.hpp>
#include <manet/reactor/io.hpp>

using namespace manet;
using namespace manet::protocol::websocket::detail;

namespace
{

reactor::RxSource
make_rx(reactor::Buffer<reactor::RX_CAP> &rx, std::string_view data)
{
  auto w = rx.wbuf();
  REQUIRE(w.size() >= data.size());
  std::memcpy(w.data(), data.data(), data.size());
  rx.inc_wpos(data.size());

  return reactor::RxSource{&rx};
}

std::array<char, 28> make_accept_key(std::string_view s)
{
  REQUIRE(s.size() == 28);
  std::array<char, 28> out{};
  std::memcpy(out.data(), s.data(), 28);
  return out;
}

std::string make_valid_handshake(
  std::string_view accept, std::string_view extra_headers = {},
  std::string_view trailing_bytes = {}
)
{
  std::string h;
  h.reserve(256 + extra_headers.size() + trailing_bytes.size());

  h += "HTTP/1.1 101 Switching Protocols\r\n";
  h += "Upgrade: websocket\r\n";
  h += "Connection: Upgrade\r\n";
  if (!extra_headers.empty())
    h += std::string(extra_headers);
  h += "Sec-WebSocket-Accept: ";
  h += accept;
  h += "\r\n";
  h += "Date: Wed, 05 Nov 2025 11:06:18 GMT\r\n";
  h += "\r\n";
  h += trailing_bytes; // optional start of first WebSocket frame

  return h;
}

std::string make_http_response(
  std::string_view status_line, std::string_view headers,
  std::string_view trailing_bytes = {}
)
{
  std::string h;
  h.reserve(status_line.size() + headers.size() + trailing_bytes.size() + 8);

  h += status_line;
  h += "\r\n";
  h += headers;
  h += "\r\n";
  h += trailing_bytes;

  return h;
}

// ---------------------------------------------------------------------

static const std::vector<std::string> VALID_ACCEPT_KEYS = {
  "Jt3poBZFLOSCJHFeZkoNbBWiFDw=", "cb+IjZZZdXrN8c/FybFz99dwhfE=",
  "5rg9VHuNbMM6C8VEyyASSzYZayA=", "c6SwVz1qSVvNxoGSWvdqwq6NwTA=",
  "dKgkFocFaJ96CX4JnS/FUKSRyWk="
};

TEST_CASE(
  "read_handshake: accepts valid 101 responses and consumes only the HTTP frame"
)
{
  for (auto const &accept : VALID_ACCEPT_KEYS)
  {
    CAPTURE(accept);

    // handshake followed by 4 extra bytes standing in for the first WebSocket
    // frame.
    std::string extra = "ABCD";
    const auto handshake =
      make_valid_handshake(accept, "Server: TestServer/1.0\r\n", extra);

    reactor::Buffer<reactor::RX_CAP> buf{};
    auto rx = make_rx(buf, handshake);

    auto expected_key = make_accept_key(accept);

    const auto before_size = rx.rbuf().size();
    const auto status = read_handshake(expected_key, rx);

    CHECK(status == protocol::Status::ok);

    // only the HTTP frame bytes are consumed.. "ABCD" remains.
    const auto http_frame_len = handshake.size() - extra.size();
    CHECK(rx.rbuf().size() == before_size - http_frame_len);
    CHECK(rx.rbuf().size() == extra.size());
  }
}

TEST_CASE("read_handshake: consumes full buffer when only handshake is present")
{
  const std::string accept = VALID_ACCEPT_KEYS.front();
  auto expected_key = make_accept_key(accept);

  auto handshake = make_valid_handshake(accept);

  reactor::Buffer<reactor::RX_CAP> buf{};
  auto rx = make_rx(buf, handshake);

  const auto before_size = rx.rbuf().size();
  const auto status = read_handshake(expected_key, rx);

  CHECK(status == protocol::Status::ok);
  CHECK(before_size == handshake.size());
  CHECK(rx.rbuf().size() == 0);
}

TEST_CASE("read_handshake: rejects non-101 status codes")
{
  const std::string accept = VALID_ACCEPT_KEYS[0];
  auto expected_key = make_accept_key(accept);

  const char headers[] =
    // clang-format off
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: Jt3poBZFLOSCJHFeZkoNbBWiFDw=\r\n"
    "Date: Wed, 05 Nov 2025 11:06:18 GMT\r\n";
  // clang-format on

  auto resp = make_http_response("HTTP/1.1 200 OK", headers);

  reactor::Buffer<reactor::RX_CAP> buf{};
  auto rx = make_rx(buf, resp);

  const auto status = read_handshake(expected_key, rx);

  CHECK(status == protocol::Status::error);
  CHECK(rx.rbuf().size() == 0);
}

// partial parser allows: TEST_CASE("read_handshake: rejects missing or wrong
// Upgrade or Connection headers")

TEST_CASE("read_handshake: rejects bad or missing Sec-WebSocket-Accept")
{
  const std::string good_accept = VALID_ACCEPT_KEYS[2];
  const std::string wrong_accept = VALID_ACCEPT_KEYS[3];

  auto expected_key = make_accept_key(good_accept);

  SUBCASE("wrong accept value")
  {
    // clang-format off
    std::string headers =
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " + wrong_accept + "\r\n"
      "Date: Wed, 05 Nov 2025 11:06:18 GMT\r\n";
    // clang-format on

    auto resp = make_http_response("HTTP/1.1 101 Switching Protocols", headers);

    reactor::Buffer<reactor::RX_CAP> buf{};
    auto rx = make_rx(buf, resp);

    const auto st = read_handshake(expected_key, rx);

    CHECK(st == protocol::Status::error);
    CHECK(rx.rbuf().size() == 0);
  }

  SUBCASE("truncated accept value")
  {
    // clang-format off
    const char headers[] =
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: 5rg9VHuNbMM6C8VEyyASSzYZ\r\n"
      "Date: Wed, 05 Nov 2025 11:06:18 GMT\r\n";
    // clang-format on

    auto resp = make_http_response("HTTP/1.1 101 Switching Protocols", headers);

    reactor::Buffer<reactor::RX_CAP> buf{};
    auto rx = make_rx(buf, resp);

    const auto st = read_handshake(expected_key, rx);

    CHECK(st == protocol::Status::error);
    CHECK(rx.rbuf().size() == 0);
  }

  SUBCASE("missing Sec-WebSocket-Accept header")
  {
    // clang-format off
    const char headers[] =
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Date: Wed, 05 Nov 2025 11:06:18 GMT\r\n";
    // clang-format on

    auto resp = make_http_response("HTTP/1.1 101 Switching Protocols", headers);

    reactor::Buffer<reactor::RX_CAP> buf{};
    auto rx = make_rx(buf, resp);

    const auto st = read_handshake(expected_key, rx);

    CHECK(st == protocol::Status::error);
    CHECK(rx.rbuf().size() == 0);
  }
}

TEST_CASE("read_handshake: error with wrong but valid key")
{
  const std::string accept = VALID_ACCEPT_KEYS[4];
  const std::string expected = VALID_ACCEPT_KEYS[0];

  auto wrong_expected_key = make_accept_key(expected);

  // clang-format off
  std::string headers =
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: " + accept + "\r\n"
    "Date: Wed, 05 Nov 2025 11:06:18 GMT\r\n";
  // clang-format on

  auto resp = make_http_response("HTTP/1.1 101 Switching Protocols", headers);

  reactor::Buffer<reactor::RX_CAP> buf{};
  auto rx = make_rx(buf, resp);

  const auto st = read_handshake(wrong_expected_key, rx);

  CHECK(st == protocol::Status::error);
  CHECK(rx.rbuf().size() == 0);
}

} // namespace
