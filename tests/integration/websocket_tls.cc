#include <doctest/doctest.h>
#include <ostream>
#include <pthread.h>
#include <sstream>
#include <vector>

#include <manet/net/epoll.hpp>
#include <manet/reactor/io.hpp>
#include <manet/transport/tls.hpp>
#include <manet/utils/hexdump.hpp>

#include <manet/protocol/websocket.hpp>

#include "websocket_test.hpp"

using Net = manet::net::Epoll;

namespace manet::protocol::websocket::test
{

/** stop after 1 message (no restart) */
struct HelloCodec : WsTest
{
  Status on_text(reactor::TxSink, std::span<const std::byte> payload) noexcept
  {
    write({reinterpret_cast<const char *>(payload.data()), payload.size()});
    signal_done();

    return Status::error;
  }
};

TEST_CASE("websocket client receives [TEXT \"Hello, World!\"]")
{
  log::set_level(log::LogLevel::trace);
  log::trace("SSL_CERT_FILE : {}", getenv("SSL_CERT_FILE"));
  ConnectionsTest<Net, WsConn<transport::tls::Tls, HelloCodec>> test("/hello");
  CHECK(test.output<0>() == Trace{"Hello, World!"});
  log::set_level(log::LogLevel::warn);
}

struct BinaryCodec : WsTest
{
  Status on_binary(reactor::TxSink, std::span<const std::byte> payload) noexcept
  {
    write(
      utils::readable_ascii(
        {reinterpret_cast<const char *>(payload.data()), payload.size()}
      )
    );

    signal_done();

    return Status::ok;
  }
};

TEST_CASE("websocket client receives binary frame")
{
  ConnectionsTest<Net, WsConn<transport::tls::Tls, BinaryCodec>> test(
    "/binary"
  );
  CHECK(test.output<0>() == Trace{"%00%01%02%03"});
}

/** stop after LIMIT messages (TEXT) */
template <std::size_t LIMIT> struct GenCodec : WsTest
{
  Status on_text(reactor::TxSink, std::span<const std::byte> payload) noexcept
  {
    write({reinterpret_cast<const char *>(payload.data()), payload.size()});

    if (output && LIMIT <= output->size())
    {
      signal_done();
    }

    return Status::ok;
  }
};

using Heartbeat = GenCodec<2>;

TEST_CASE("stay alive long alive to answer a bunch of PING with PONG")
{
  ConnectionsTest<Net, WsConn<transport::tls::Tls, Heartbeat>> test(
    "/heartbeat"
  );

  auto const &out = test.output<0>();
  CHECK(out.size() == 2);
  CHECK(out[0] == "🫀");
  CHECK(out[1] == "🫀");
}

} // namespace manet::protocol::websocket::test
