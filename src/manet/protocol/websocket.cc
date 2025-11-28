#include <cstring>
#include <openssl/sha.h>
#include <random>

#include "manet/protocol/websocket.hpp"
#include "manet/utils/base64.hpp"
#include "manet/utils/logging.hpp"

namespace manet::protocol::websocket_detail
{

void random_bytes(std::byte *buf, std::size_t len) noexcept
{
  std::random_device rd;
  for (std::size_t i = 0; i < len; ++i)
  {
    buf[i] = static_cast<std::byte>(rd());
  }
}

Handshake make_handshake(
  std::string_view host, std::string_view path,
  std::span<const websocket::Header> extra
) noexcept
{
  // generate nonce
  std::array<std::byte, 16> nonce{};
  random_bytes(nonce.data(), nonce.size());

  std::array<char, 24> key_b64{};
  utils::base64_encode<16, 24>(nonce, key_b64);

  std::string req;
  req.reserve(512);

  // build request line & headers
  std::format_to(
    std::back_inserter(req),
    "GET {} HTTP/1.1\r\n" "Host: {}\r\n" "Upgrade: websocket\r\n" "Connection" ": " "Upgrade\r" "\n" "Sec-" "WebSo" "cket-" "Key: " "{}" "\r\n" "Sec-WebSocket-Version: 13\r\n",
    path, host,
    std::string_view(
      reinterpret_cast<const char *>(key_b64.data()), key_b64.size()
    )
  );

  for (const auto &h : extra)
  {
    if (!h.name.empty())
    {
      std::format_to(std::back_inserter(req), "{}: {}\r\n", h.name, h.value);
    }
  }
  req.append("\r\n");

  // Sec-WebSocket-Accept = base64( SHA1( key_b64 ; GUID ) )
  std::array<uint8_t, 24 + 36> scratch{};
  static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

  // key_b64
  std::memcpy(scratch.data(), key_b64.data(), key_b64.size());
  // + GUID  (excl. \0)
  std::memcpy(scratch.data() + key_b64.size(), kGuid, sizeof(kGuid) - 1);

  // SHA1( scratch )
  std::array<std::byte, 20> sha1{};
  SHA1(
    scratch.data(), key_b64.size() + (sizeof(kGuid) - 1),
    reinterpret_cast<uint8_t *>(sha1.data())
  );

  // base64(20) -> 28
  std::array<char, 28> accept{};
  utils::base64_encode(sha1, accept);

  return Handshake{.upgrade_request = std::move(req), .ws_accept_key = accept};
}

/** counter:
 * - 0: ()
 * - 1: (\r)
 * - 2: (\r\n)
 * - 3: (\r\n\r)
 * - 4: (\r\n\r\n)
 */
uint8_t advance_crlf_count(uint8_t crlf_counter, std::byte chr)
{
  if (chr == std::byte{'\r'})
  {
    return crlf_counter + (crlf_counter % 2 == 0 ? 1 : 0);
  }
  else if (chr == std::byte{'\n'})
  {
    return crlf_counter + (crlf_counter % 2);
  }
  else
  {
    return 0;
  }
}

Status
read_handshake(std::array<char, 28> ws_accept_key, reactor::IO io) noexcept
{
  auto input = io.rbuf();

  // establish indices (HTTP response code, Sec-WebSocket-Accept header, HTTP
  // response end)
  uint8_t crlf_counter = 0;
  std::size_t sp_ix = 0;
  std::size_t ws_key_ix = 0;

  std::size_t i = 0;

  for (std::byte chr : input)
  {
    if (sp_ix == 0 && chr == std::byte{' '})
    {
      sp_ix = i;
    }

    // after a \r\n (crlf_counter == 2)
    constexpr std::string_view h = "Sec-WebSocket-Accept:";

    if (ws_key_ix == 0 && crlf_counter == 2 && chr == std::byte{'S'} &&
        i + h.size() < input.size())
    {
      // check for Sec-WebSocket-Accept
      auto sv = std::string_view(
        reinterpret_cast<const char *>(input.data()) + i, h.size()
      );

      if (std::equal(
            sv.begin(), sv.end(), h.begin(), h.end(),
            [](unsigned char a, unsigned char b)
            { return std::tolower(a) == std::tolower(b); }
          ))
      {
        // found the header starting at i, seek index of value (skip SP)
        ws_key_ix = i + h.size();
        for (; input[ws_key_ix] == std::byte{' '}; ws_key_ix++)
          ;
      }
    }

    // count through \r\n\r\n
    crlf_counter = advance_crlf_count(crlf_counter, chr);

    // hit \r\n\r\n  -> we read the complete HTTP message
    if (crlf_counter == 4)
    {
      break;
    }

    i++;
  }

  // dump response
  if constexpr (utils::logging_enabled)
  {
    std::string_view sv(reinterpret_cast<const char *>(input.data()), i + 1);
    utils::trace("WebSocket handshake:\n{}", sv);
  }

  // validate the HTTP response

  if (crlf_counter != 4)
  {
    utils::error("invalid HTTP response: unexpected end.");
    return Status::error;
  }

  if (sp_ix == 0 || input.size() <= sp_ix + 3)
  {
    utils::error("invalid HTTP response: status line.");
    return Status::error;
  }

  // make sure we got `HTTP-Version SP 101...`
  if (input[sp_ix + 1] != std::byte{'1'} ||
      input[sp_ix + 2] != std::byte{'0'} || input[sp_ix + 3] != std::byte{'1'})
  {
    utils::error(
      "HTTP error: {}{}{}", static_cast<char>(input[sp_ix + 1]),
      static_cast<char>(input[sp_ix + 2]), static_cast<char>(input[sp_ix + 3])
    );
    return Status::error;
  }

  if (ws_key_ix != 0)
  {
    std::span<const std::byte> got = input.subspan(ws_key_ix, 28);

    auto got_sv =
      std::string_view(reinterpret_cast<const char *>(got.data()), got.size());

    auto expect_sv = std::string_view(
      reinterpret_cast<const char *>(ws_accept_key.data()), ws_accept_key.size()
    );

    if (got_sv != expect_sv)
    {
      utils::error(
        "WebSocket error: Sec-WebSocket-Accept (expected: {}, got: {})",
        expect_sv, got_sv
      );
      return Status::error;
    }
  }

  // we consumed (i+1) bytes!
  io.read(i + 1);

  return Status::ok;
}

} // namespace manet::protocol::websocket_detail
