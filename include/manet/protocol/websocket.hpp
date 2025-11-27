#pragma once

#include <vector>

/*
#include "net/io.hpp"
#include "net/status.hpp"
#include "net/utils/hexdump.hpp"
#include "net/utils/logging.hpp"

#include "websocket_frame.hpp"

struct header
{
  std::string name;
  std::string value;
};

struct websocket_args
{
  std::string path;
  std::vector<header> extra;
};

enum class websocket_state : uint8_t
{
  handshake_sent,
  listening
};

struct websocket_ctx
{
  static constexpr std::size_t MSG_CAP = 1 << 20;

  std::array<std::byte, MSG_CAP> msg_buf{};
  std::size_t msg_len = 0;
  ws::opcode opcode;

  websocket_state state = websocket_state::handshake_sent;

  std::string_view host;

  std::string path;
  std::vector<header> extra;

  std::array<char, 28> ws_accept_key;
};

template <typename Codec>
concept HasTextHandler = requires { (void)&Codec::on_text; };

template <typename Codec>
concept HasBinaryHandler = requires { (void)&Codec::on_binary; };

template <class Codec> struct ws_traits
{
  using ctx_t = typename Codec::ctx_t;
  using codec_args_t = typename Codec::args_t;

  struct args_t
  {
    websocket_args ws{};
    codec_args_t codec{};

    args_t() = default;
    args_t(websocket_args w, codec_args_t c = {})
        : ws(std::move(w)), codec(std::move(c))
    {
    }
  };
};

template <class Codec> using ws_ctx_t = typename ws_traits<Codec>::ctx_t;

template <class Codec> using ws_args_t = typename ws_traits<Codec>::args_t;

struct websocket_handshake
{
  std::string upgrade_request;
  std::array<char, 28> ws_accept_key;
};

websocket_handshake ws_handshake(
  std::string_view host, std::string_view path, std::span<const header> extra
) noexcept;

void random_bytes(std::byte *buf, std::size_t len) noexcept;

protocol_status
read_handshake(std::array<char, 28> ws_accept_key, IO io) noexcept;

template <typename Codec>
protocol_status handle_msg(
  ws_ctx_t<Codec> &ctx, IO io, ws::opcode opcode,
  std::span<const std::byte> payload
) noexcept
{
  switch (opcode)
  {
  case ws::opcode::ping:
  {
    std::size_t len = payload.size();
    if (126 <= len)
    {
      // FIXME: handle longer payloads
      Log::log(LogLevel::error, "PING payload (>= 126 bits)");
      return protocol_status::close;
    }
    else
    {
      Log::log(LogLevel::trace, "WebSocket::PING");
    }

    auto out = io.wbuf();

    if (out.size() < 6 + len)
    {
      return protocol_status::close;
    }

    // FIN | opcode
    out.data()[0] = std::byte{0x80} | static_cast<std::byte>(ws::opcode::pong);
    // MASK=1 | payload len
    out.data()[1] = std::byte{0x80} | static_cast<std::byte>(len);

    // mask
    random_bytes(out.data() + 2, 4);

    // masked payload
    for (std::size_t i = 0; i < payload.size(); i++)
    {
      out.data()[6 + i] = payload.data()[i] ^ out.data()[2 + (i & 3)];
    }

    io.wrote(6 + len);

    return protocol_status::ok;
  }
  case ws::opcode::close:
  {
    Log::log(LogLevel::info, "WebSocket::CLOSE");

    auto out = io.wbuf();
    const std::size_t len = payload.size();

    uint16_t code = uint16_t(ws::close_code::normal);
    if (len >= 2)
    {
      const uint8_t *p = reinterpret_cast<const uint8_t *>(payload.data());
      code = (uint16_t(p[0]) << 8) | uint16_t(p[1]);
    }

    if (out.size() >= 8)
    {
      out.data()[0] =
        std::byte{0x80} | static_cast<std::byte>(ws::opcode::close);
      out.data()[1] = std::byte{0x80 | 0x02}; // MASK; len = 2 bytes

      out.data()[2] = std::byte{0xDE};
      out.data()[3] = std::byte{0xAD};
      out.data()[4] = std::byte{0xBE};
      out.data()[5] = std::byte{0xEF};

      std::byte code_bytes[2] = {std::byte(code >> 8), std::byte(code & 0xff)};

      out.data()[6] = std::byte(code >> 8) ^ std::byte{0xDE};
      out.data()[7] = std::byte(code & 0xff) ^ std::byte{0xAD};

      io.wrote(8);
    }

    return protocol_status::close;
  }
  case ws::opcode::text:
  {
    Log::log(LogLevel::trace, "WebSocket::TEXT");
    if constexpr (HasTextHandler<Codec>)
    {
      return Codec::on_text(ctx, io, payload);
    }
    else
    {
      return protocol_status::ok;
    }
  }
  case ws::opcode::binary:
  {
    Log::log(LogLevel::trace, "WebSocket::BINARY");
    if constexpr (HasBinaryHandler<Codec>)
    {
      return Codec::on_binary(ctx, io, payload);
    }
    else
    {
      return protocol_status::ok;
    }
  }
  }

  return protocol_status::ok;
}

template <typename Codec>
inline protocol_status read_frame(ws_ctx_t<Codec> &ctx, IO io) noexcept
{
  auto input = io.rbuf();

  // attempt reading the frame
  ws::parse_output parsed;

  switch (ws::parse_frame(input, parsed))
  {
  case ws::parse_status::ok:
  {
    // successful parse: read bytes and advance input
    io.read(parsed.consumed);

    auto payload = parsed.frame.payload;

    // An unfragmented message consists of a single frame with the FIN
    // bit set (Section 5.2) and an opcode other than 0.

    // A fragmented message consists of a single frame with the FIN bit
    // clear and an opcode other than 0, followed by zero or more frames
    // with the FIN bit clear and the opcode set to 0, and terminated by
    // a single frame with the FIN bit set and an opcode of 0.
    if (!parsed.frame.fin || parsed.frame.op == ws::opcode::cont)
    {
      if (ctx.MSG_CAP < ctx.msg_len + payload.size())
      {
        Log::log(LogLevel::error, "msg buffer overflow");
        return protocol_status::error;
      }

      if (parsed.frame.payload_len != payload.size())
      {
        Log::log(LogLevel::error, "unexpected payload size");
        return protocol_status::error;
      }

      memcpy(ctx.msg_buf.data() + ctx.msg_len, payload.data(), payload.size());
      ctx.msg_len += payload.size();

      if (parsed.frame.fin)
      {
        auto status = handle_msg<Codec>(
          ctx, io, ctx.opcode,
          std::span<const std::byte>{ctx.msg_buf}.first(ctx.msg_len)
        );

        // clear message buffer
        ctx.msg_len = 0;

        if (status != protocol_status::ok)
        {
          return status;
        }
      }
      else if (parsed.frame.op != ws::opcode::cont)
      {
        ctx.opcode = parsed.frame.op;
      }
    }
    else
    {
      auto status = handle_msg<Codec>(ctx, io, parsed.frame.op, payload);

      if (status != protocol_status::ok)
      {
        return status;
      }
    }

    return protocol_status::ok;
  }
  case ws::parse_status::need_more:
  {
    Log::log(
      LogLevel::trace, "need more, rxbuf[{}]:\n{}", input.size(), hexdump(input)
    );
    return protocol_status::ok;
  }
  case ws::parse_status::masked_server:
  {
    Log::log(LogLevel::error, "server-to-client frame must not be masked");
    return protocol_status::error;
  }
  case ws::parse_status::bad_reserved:
  {
    Log::log(LogLevel::error, "RSV bits set");
    return protocol_status::error;
  }
  }

  return protocol_status::error;
}

std::size_t
ws_write_close(ws::close_code code, std::span<std::byte> output) noexcept;

template <typename Codec> struct WebSocket
{
  using args_t = ws_args_t<Codec>;
  using ctx_t = ws_ctx_t<Codec>;

  static ctx_t
  init(std::string_view host, uint16_t port, const args_t &args) noexcept
  {
    ctx_t ctx{};
    auto &base = static_cast<websocket_ctx &>(ctx);

    base.host = host;
    base.path = args.ws.path;
    base.extra = args.ws.extra;

    if constexpr (requires { Codec::init(ctx, host, port, args.codec); })
    {
      Codec::init(ctx, host, port, args.codec);
    }

    return ctx;
  };

  static protocol_status on_connect(ctx_t &ctx, TxSink output) noexcept
  {
    auto handshake = ws_handshake(ctx.host, ctx.path, ctx.extra);
    auto out = output.wbuf();

    auto len = handshake.upgrade_request.size();
    if (out.size() < len)
    {
      return protocol_status::error;
    }

    ctx.ws_accept_key = handshake.ws_accept_key;
    ctx.state = websocket_state::handshake_sent;

    std::memcpy(out.data(), handshake.upgrade_request.data(), len);
    output.wrote(len);

    return protocol_status::ok;
  }

  static protocol_status on_data(ctx_t &ctx, IO io) noexcept
  {
    switch (ctx.state)
    {
    case websocket_state::handshake_sent:
    {
      auto status = read_handshake(ctx.ws_accept_key, io);

      if (status == protocol_status::ok)
      {
        ctx.state = websocket_state::listening;
      }

      return status;
    }
    case websocket_state::listening:
      return read_frame<Codec>(ctx, io);
    }

    return protocol_status::error;
  }

  static protocol_status on_shutdown(ctx_t &ctx, TxSink output) noexcept
  {
    auto sent = ws_write_close(ws::close_code::normal, output.wbuf());
    output.wrote(sent);

    return 0 < sent ? protocol_status::close : protocol_status::error;
  }
};
*/
