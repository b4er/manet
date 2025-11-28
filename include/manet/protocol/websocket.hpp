#pragma once

#include <cstring>
#include <vector>

#include "manet/protocol/status.hpp"
#include "manet/reactor/io.hpp"
#include "manet/utils/hexdump.hpp"
#include "manet/utils/logging.hpp"

#include "websocket_frame.hpp"

namespace manet::protocol
{

namespace websocket
{

struct Header
{
  std::string name;
  std::string value;
};

} // namespace websocket

namespace websocket_detail
{

struct Handshake
{
  std::string upgrade_request;
  std::array<char, 28> ws_accept_key;
};

Handshake make_handshake(
  std::string_view host, std::string_view path,
  std::span<const websocket::Header> extra
) noexcept;

Status
read_handshake(std::array<char, 28> ws_accept_key, reactor::IO io) noexcept;
void random_bytes(std::byte *buf, std::size_t len) noexcept;

} // namespace websocket_detail

template <typename Codec> struct WebSocket
{
  struct config_t
  {
    std::string path;
    std::vector<websocket::Header> extra;

    typename Codec::config_t codec_config{};
  };

  struct Session
  {
    static constexpr std::size_t MSG_CAP = 1 << 20;

    std::string_view host;
    std::string path;
    std::vector<websocket::Header> extra;

    std::size_t msg_len = 0;

    std::array<char, 28> ws_accept_key{};

    websocket::OpCode opcode;

    enum class State : uint8_t
    {
      idle,
      handshake_sent,
      listening
    } state = State::idle;

    std::array<std::byte, MSG_CAP> msg_buf{};

    Codec codec;

    Session(
      const std::string &host, uint16_t /*port*/, config_t config
    ) noexcept
        : host(host),
          path(config.path),
          extra(config.extra),
          codec(std::move(config.codec_config))
    {
    }

    Status on_connect(reactor::TxSink output) noexcept
    {
      auto handshake = websocket_detail::make_handshake(host, path, extra);
      auto out = output.wbuf();

      auto len = handshake.upgrade_request.size();
      if (out.size() < len)
      {
        return Status::error;
      }

      ws_accept_key = handshake.ws_accept_key;
      state = State::handshake_sent;

      std::memcpy(out.data(), handshake.upgrade_request.data(), len);
      output.wrote(len);

      return Status::ok;
    }

    Status on_data(reactor::IO io) noexcept
    {
      switch (state)
      {
      case State::handshake_sent:
      {
        auto status = websocket_detail::read_handshake(ws_accept_key, io);
        if (status == Status::ok)
        {
          state = State::listening;
        }

        return status;
      }
      case State::listening:
        return dispatch_frame(io);
      }

      return Status::error;
    }

    Status on_shutdown([[maybe_unused]] reactor::TxSink output) noexcept
    {
      // auto sent = ws_write_close(ws::close_code::normal, output.wbuf());
      // output.wrote(sent);

      // return 0 < sent ? Status::close : Status::error;
      return Status::ok;
    }

  private:
    Status dispatch_frame(reactor::IO io) noexcept
    {
      auto input = io.rbuf();

      // attempt reading the frame
      websocket::parse_output parsed;

      switch (websocket::parse_frame(input, parsed))
      {
      case websocket::parse_status::ok:
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
        if (!parsed.frame.fin || parsed.frame.op == websocket::OpCode::cont)
        {
          if (MSG_CAP < msg_len + payload.size())
          {
            utils::error("msg buffer overflow");
            return Status::error;
          }

          if (parsed.frame.payload_len != payload.size())
          {
            utils::error("unexpected payload size");
            return Status::error;
          }

          memcpy(msg_buf.data() + msg_len, payload.data(), payload.size());
          msg_len += payload.size();

          if (parsed.frame.fin)
          {
            auto status = handle_frame(
              io, opcode, std::span<const std::byte>{msg_buf}.first(msg_len)
            );

            // clear message buffer
            msg_len = 0;

            if (status != Status::ok)
            {
              return status;
            }
          }
          else if (parsed.frame.op != websocket::OpCode::cont)
          {
            opcode = parsed.frame.op;
          }
        }
        else
        {
          auto status = handle_frame(io, parsed.frame.op, payload);

          if (status != Status::ok)
          {
            return status;
          }
        }

        return Status::ok;
      }
      case websocket::parse_status::need_more:
      {
        utils::trace(
          "need more, rxbuf[{}]:\n{}", input.size(), utils::hexdump(input)
        );
        return Status::ok;
      }
      case websocket::parse_status::masked_server:
      {
        utils::error("server-to-client frame must not be masked");
        return Status::error;
      }
      case websocket::parse_status::bad_reserved:
      {
        utils::error("RSV bits set");
        return Status::error;
      }
      }

      return Status::error;
    }

    Status handle_frame(
      reactor::IO io, websocket::OpCode opcode,
      std::span<const std::byte> payload
    ) noexcept
    {
      switch (opcode)
      {
      case websocket::OpCode::cont:
      {
        utils::warn("WebSocket::CONT nothing to handle");
        return Status::ok;
      }
      case websocket::OpCode::text:
      {
        utils::trace("WebSocket::TEXT");
        if constexpr (requires { (void)&Codec::on_text; })
        {
          return codec.on_text(io, payload);
        }

        return Status::ok;
      }
      case websocket::OpCode::binary:
      {
        utils::trace("WebSocket::BINARY");
        if constexpr (requires { (void)&Codec::on_binary; })
        {
          return codec.on_binary(io, payload);
        }
        else
        {
          return Status::ok;
        }
      }
      case websocket::OpCode::close:
      {
        utils::info("WebSocket::CLOSE");

        auto out = io.wbuf();
        const std::size_t len = payload.size();

        uint16_t code = uint16_t(websocket::CloseCode::normal);
        if (len >= 2)
        {
          const uint8_t *p = reinterpret_cast<const uint8_t *>(payload.data());
          code = (uint16_t(p[0]) << 8) | uint16_t(p[1]);
        }

        if (out.size() >= 8)
        {
          out.data()[0] =
            std::byte{0x80} | static_cast<std::byte>(websocket::OpCode::close);
          out.data()[1] = std::byte{0x80 | 0x02}; // MASK; len = 2 bytes

          out.data()[2] = std::byte{0xDE};
          out.data()[3] = std::byte{0xAD};
          out.data()[4] = std::byte{0xBE};
          out.data()[5] = std::byte{0xEF};

          out.data()[6] = std::byte(code >> 8) ^ std::byte{0xDE};
          out.data()[7] = std::byte(code & 0xff) ^ std::byte{0xAD};

          io.wrote(8);
        }

        return Status::close;
      }
      case websocket::OpCode::ping:
      {
        std::size_t len = payload.size();
        if (126 <= len)
        {
          // FIXME: handle longer payloads
          utils::error("PING payload (>= 126 bits)");
          return Status::close;
        }
        else
        {
          utils::trace("WebSocket::PING");
        }

        auto out = io.wbuf();

        if (out.size() < 6 + len)
        {
          return Status::close;
        }

        // FIN | opcode
        out.data()[0] =
          std::byte{0x80} | static_cast<std::byte>(websocket::OpCode::pong);
        // MASK=1 | payload len
        out.data()[1] = std::byte{0x80} | static_cast<std::byte>(len);

        // mask
        websocket_detail::random_bytes(out.data() + 2, 4);

        // masked payload
        for (std::size_t i = 0; i < payload.size(); i++)
        {
          out.data()[6 + i] = payload.data()[i] ^ out.data()[2 + (i & 3)];
        }

        io.wrote(6 + len);

        return Status::ok;
      }
      case websocket::OpCode::pong:
      {
        return Status::ok;
      }
      }
    }
  };
};

/*
template <typename Codec>
concept HasTextHandler = requires { (void)&Codec::on_text; };

template <typename Codec>
concept HasBinaryHandler = requires { (void)&Codec::on_binary; };

std::size_t
ws_write_close(ws::close_code code, std::span<std::byte> output) noexcept;

*/

} // namespace manet::protocol
