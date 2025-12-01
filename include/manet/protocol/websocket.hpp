#pragma once

#include <cstring>
#include <vector>

#include "manet/logging.hpp"
#include "manet/protocol/status.hpp"
#include "manet/reactor/io.hpp"
#include "manet/utils/hexdump.hpp"

#include "websocket_concepts.hpp"
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

namespace detail
{

struct Handshake
{
  std::string upgrade_request;
  std::array<char, 28> ws_accept_key;
};

Handshake make_handshake(
  std::string_view host, std::string_view path, std::span<const Header> extra
) noexcept;

Status read_handshake(
  std::array<char, 28> ws_accept_key, reactor::RxSource in
) noexcept;

std::size_t write_control_frame(
  std::span<std::byte> output, OpCode opcode, std::span<const std::byte> payload
) noexcept;
std::size_t write_close(std::span<std::byte> output, CloseCode code) noexcept;

void random_bytes(std::byte *buf, std::size_t len) noexcept;

} // namespace detail

template <typename Codec>
  requires MessageCodec<Codec>
struct WebSocket
{
  struct config_t
  {
    std::string path;
    std::vector<Header> extra;

    typename Codec::config_t codec_config{};
  };

  struct Session
  {
    static constexpr std::size_t MSG_CAP = 1 << 20;

    std::string_view host;
    std::string path;
    std::vector<Header> extra;

    std::size_t msg_len = 0;

    std::array<char, 28> ws_accept_key{};

    detail::OpCode opcode;

    enum class State : uint8_t
    {
      idle,
      handshake_sent,
      listening
    } state = State::idle;

    std::array<std::byte, MSG_CAP> msg_buf{};

    Codec codec;

    Session(std::string_view host, uint16_t /*port*/, config_t &config) noexcept
        : host(host),
          path(config.path),
          extra(config.extra),
          codec(config.codec_config)
    {
    }

    Status on_connect(reactor::IO output) noexcept
    {
      auto handshake = detail::make_handshake(host, path, extra);
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
        auto before = io.rbuf().size();

        auto status = detail::read_handshake(ws_accept_key, io);

        // if we consumed the HTTP frame and are ok then start listening
        if (io.rbuf().size() != before && status == Status::ok)
        {
          state = State::listening;
        }

        return status;
      }
      case State::listening:
        return dispatch_frame(io);

      default:
        break;
      }

      return Status::error;
    }

    Status on_shutdown(reactor::IO io) noexcept
    {
      detail::CloseCode close_code;

      if constexpr (HasShutdownHandler<Codec>)
      {
        close_code = codec.on_shutdown();
      }
      else
      {
        close_code = detail::CloseCode::normal;
      }

      auto sent = detail::write_close(io.wbuf(), close_code);
      io.wrote(sent);

      return 0 < sent ? Status::close : Status::error;
    }

    void heartbeat(reactor::TxSink out) noexcept
    {
      std::span<const std::byte> payload{};
      out.wrote(
        detail::write_control_frame(out.wbuf(), detail::OpCode::ping, payload)
      );
    }

  private:
    Status dispatch_frame(reactor::IO io) noexcept
    {
      auto input = io.rbuf();

      // attempt reading the frame
      detail::parse_output parsed;

      switch (detail::parse_frame(input, parsed))
      {
      case detail::parse_status::ok:
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
        if (!parsed.frame.fin || parsed.frame.op == detail::OpCode::cont)
        {
          if (MSG_CAP < msg_len + payload.size())
          {
            log::error("msg buffer overflow");
            return Status::error;
          }

          if (parsed.frame.payload_len != payload.size())
          {
            log::error("unexpected payload size");
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
          else if (parsed.frame.op != detail::OpCode::cont)
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
      case detail::parse_status::need_more:
      {
        log::trace(
          "need more, rxbuf[{}]:\n{}", input.size(), utils::hexdump(input)
        );
        return Status::ok;
      }
      case detail::parse_status::masked_server:
      {
        log::error("server-to-client frame must not be masked");
        return Status::error;
      }
      case detail::parse_status::bad_reserved:
      {
        log::error("RSV bits set");
        return Status::error;
      }
      }

      return Status::error;
    }

    Status handle_frame(
      reactor::IO io, detail::OpCode opcode, std::span<const std::byte> payload
    ) noexcept
    {
      switch (opcode)
      {
      case detail::OpCode::cont:
      {
        log::warn("WebSocket::CONT nothing to handle");
        break;
      }
      case detail::OpCode::text:
      {
        log::trace("WebSocket::TEXT");
        if constexpr (HasTextHandler<Codec>)
        {
          return codec.on_text(io, payload);
        }

        return Status::ok;
      }
      case detail::OpCode::binary:
      {
        log::trace("WebSocket::BINARY");
        if constexpr (HasBinaryHandler<Codec>)
        {
          return codec.on_binary(io, payload);
        }
        else
        {
          return Status::ok;
        }
      }
      case detail::OpCode::close:
      {
        log::info("WebSocket::CLOSE");

        auto out = io.wbuf();
        const std::size_t len = payload.size();

        uint16_t code = uint16_t(detail::CloseCode::normal);
        if (len >= 2)
        {
          const uint8_t *p = reinterpret_cast<const uint8_t *>(payload.data());
          code = (uint16_t(p[0]) << 8) | uint16_t(p[1]);
        }

        if (out.size() >= 8)
        {
          out.data()[0] =
            std::byte{0x80} | static_cast<std::byte>(detail::OpCode::close);
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
      case detail::OpCode::ping:
      {
        std::size_t len = payload.size();
        if (126 <= len)
        {
          // FIXME: handle longer payloads
          log::error("PING payload (>= 126 bits)");
          return Status::close;
        }
        else
        {
          log::trace("WebSocket::PING");
        }

        auto out = io.wbuf();

        if (out.size() < 6 + len)
        {
          return Status::close;
        }

        io.wrote(
          detail::write_control_frame(out, detail::OpCode::pong, payload)
        );

        return Status::ok;
      }
      case detail::OpCode::pong:
      {
        break;
      }
      }

      return Status::ok;
    }
  };
};

} // namespace websocket

} // namespace manet::protocol
