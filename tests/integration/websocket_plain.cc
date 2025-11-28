#include <doctest/doctest.h>
#include <ostream>
#include <pthread.h>
#include <sstream>
#include <vector>

#include <manet/net/epoll.hpp>
#include <manet/reactor/io.hpp>
#include <manet/transport/plain.hpp>
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
  ConnectionsTest<Net, WsConn<transport::Plain, HelloCodec>> test("/hello");
  CHECK(test.output<0>() == Trace{"Hello, World!"});
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
  ConnectionsTest<Net, WsConn<transport::Plain, BinaryCodec>> test("/binary");
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
using Counter = GenCodec<20>;

TEST_CASE("stay alive long alive to answer a bunch of PING with PONG")
{
  ConnectionsTest<Net, WsConn<transport::Plain, Heartbeat>> test("/heartbeat");

  auto const &out = test.output<0>();
  CHECK(out.size() == 2);
  CHECK(out[0] == "🫀");
  CHECK(out[1] == "🫀");
}

TEST_CASE("websocket client auto-restarts and receives multiple counter cycles")
{
  ConnectionsTest<Net, WsConn<transport::Plain, Counter>> test("/counter");

  auto const &out = test.output<0>();

  CHECK(out.size() == 20);

  for (std::size_t i = 0; i < 20; i++)
  {
    CHECK(out[i] == ("counter=" + std::to_string(i % 10)));
  }
}

TEST_CASE("multiple websocket connections share the same reactor")
{
  ConnectionsTest<
    Net, WsConn<transport::Plain, HelloCodec>,
    WsConn<transport::Plain, Counter>>
    test(std::make_tuple("/hello", "/counter"));

  CHECK(test.output<0>() == Trace{"Hello, World!"});
  CHECK(test.output<1>().size() >= 10);
}

} // namespace manet::protocol::websocket::test
