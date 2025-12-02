#include <cstdint>
#include <string_view>
#include <tuple>
#include <variant>

#include "manet/reactor/io.hpp"
#include "manet/transport/plain.hpp"

#include "mock/net.hpp"
#include "mock/reactor.hpp"

namespace manet::protocol
{

inline Status
reply(reactor::IO io, uint8_t op, std::span<const std::byte> payload) noexcept
{
  auto out = io.wbuf();

  if (out.size() < 2 + payload.size())
  {
    return Status::error;
  }

  // write frame (see framing below)

  out.data()[0] = std::byte{op};
  out.data()[1] = static_cast<std::byte>(payload.size());

  std::memcpy(out.data() + 2, payload.data(), payload.size());

  io.wrote(2 + payload.size());

  return Status::ok;
}

/**
 * simple protocol: [ opcode: 1 byte ] [ len: 1 byte ] [ payload: len ]
 *
 * opcode 0x01 = TEXT
 * opcode 0x08 = CLOSE
 * opcode 0x42 = MULTI-CLOSE
 */
struct CloseTest
{
  using config_t = std::monostate;

  struct Session
  {
    enum class mode : uint8_t
    {
      Close,
      MultiClose
    };

    bool saw_close = false;
    mode shutdown = mode::Close;

    Session(std::string_view, uint16_t, config_t) noexcept {}

    /** on_data consumes 1 frame */
    Status on_data(reactor::IO io) noexcept
    {
      auto in = io.rbuf();
      auto out = io.wbuf();

      if (in.size() < 2)
        return Status::ok; // need more

      auto op = static_cast<uint8_t>(in[0]);
      auto len = static_cast<uint8_t>(in[1]);

      if (in.size() < static_cast<std::size_t>(2 + len))
        return Status::ok; // need more

      // payload
      auto payload = std::span<const std::byte>{in.data() + 2, len};
      io.read(2 + len); // payload is still valid during this `on_data` call!

      if (op == 0x01) // TEXT
      {
        if (out.size() < len)
          return Status::error;

        return reply(io, op, payload);
      }

      if (op == 0x08) // CLOSE
      {
        saw_close = true;
        shutdown = mode::Close;

        auto st = reply(io, op, payload);
        return st == Status::ok ? Status::close : st;
      }

      if (op == 0x42)
      {
        saw_close = true;
        shutdown = mode::MultiClose;

        if (out.size() < len)
        {
          return Status::error;
        }

        (void)reply(io, op, payload);

        return Status::close;
      }

      return Status::error;
    }

    Status on_shutdown(reactor::IO io) noexcept
    {
      if (shutdown == mode::Close)
      {
        return Status::close;
      }

      auto in = io.rbuf();
      auto out = io.wbuf();

      if (in.size() < 2)
        return Status::ok; // need more

      auto op = static_cast<uint8_t>(in[0]);
      auto len = static_cast<uint8_t>(in[1]);

      if (in.size() < static_cast<std::size_t>(2 + len))
        return Status::ok; // need more

      auto payload = std::span<const std::byte>{in.data() + 2, len};
      io.read(2 + len);

      switch (op)
      {
      case 0x01: // TEXT during shutdown
      case 0x42: // MULTI-CLOSE during shutdown
      {
        if (out.size() < len)
          return Status::error;

        // keep echoing but remain in CloseProtocol
        return reply(io, op, payload);
      }

      case 0x08: // final CLOSE terminates shutdown
      {
        saw_close = true;

        if (out.size() < len)
          return Status::error;

        (void)reply(io, op, payload);
        return Status::close; // -> DrainProtocol
      }

      default:
        return Status::error;
      }
    }
  };
};

} // namespace manet::protocol

void reactor_test1_fragmented(
  std::initializer_list<std::string_view> inputs,
  std::string_view expected_output
)
{
  for (int i = 0; i < 2; i++)
  {
    auto actions = gen_script(inputs);

    std::string input = "";
    for (auto str : inputs)
    {
      input.append(str);
    }

    // run with Plain transport
    test1<manet::transport::Plain, manet::protocol::CloseTest>(
      i != 0, input, expected_output, actions
    );

    // run with ScriptedTransport (should be the almost same call structure as
    // Plain)
    auto transport_script = happypath(inputs);

    test1<ScriptedTransport, manet::protocol::CloseTest>(
      i != 0, input, expected_output, actions, &transport_script
    );
  }
}

auto close_test = test1<manet::transport::Plain, manet::protocol::CloseTest>;

auto R = FdAction::GrantRead;
auto W = FdAction::GrantWrite;

TEST_CASE("<CloseTest> closes eagerly (ET test)")
{
  // [CLOSE "a"] at once
  reactor_test1_fragmented({"\x08\x01" "a"}, "\x08\x01" "a");
  // [TEXT "hi"] [CLOSE "aa"] at once
  reactor_test1_fragmented(
    {"\x01\x02" "hi" "\x08\x02" "aa"}, "\x01\x02" "hi" "\x08\x02" "aa"
  );
  // [TEXT "hi"] [CLOSE "aaa"] fragmented (a fragment per frame)
  reactor_test1_fragmented(
    {"\x01\x02" "hi", "\x08\x03" "aaa"}, "\x01\x02" "hi" "\x08\x03" "aaa"
  );
  // [TEXT "hi"] [CLOSE "aaa"] fragmented (3 fragments)
  reactor_test1_fragmented(
    {"\x01\x02" "hi", "\x08\x03" "a", "aa"}, "\x01\x02" "hi" "\x08\x03" "aaa"
  );
  // [TEXT "a"] [TEXT "bc"] [CLOSE "xyz"] at once
  reactor_test1_fragmented(
    {"\x01\x01" "a" "\x01\x02" "bc" "\x08\x03" "xyz"},
    "\x01\x01" "a" "\x01\x02" "bc" "\x08\x03" "xyz"
  );
  // [TEXT "a"] [TEXT "bc"] [CLOSE "xyz"] but interleaved (4 fragments)
  reactor_test1_fragmented(
    {"\x01\x01" "a", "\x01\x02" "b", "c\x08\x03" "x", "yz"},
    "\x01\x01" "a" "\x01\x02" "bc" "\x08\x03" "xyz"
  );
}

TEST_CASE("<CloseTest> CLOSE with zero-length payload")
{
  // [TEXT ""]
  constexpr char close_frame_raw[] = {'\x08', '\x00'};
  std::string_view frame(close_frame_raw, 2);

  reactor_test1_fragmented({frame}, frame);
}

TEST_CASE("<Plain,CloseTest> invalid opcode triggers error")
{
  // [?? "x"]
  auto outputs = close_test(false, "\xFF\x01" "x", "", {R(3)}, {}, {});

  CHECK_MESSAGE(outputs.all_done, "connections should be done");
  CHECK_MESSAGE(
    outputs.restarts.size() == 0, "Connection should not be restarted"
  );
}

TEST_CASE("<Plain,CloseTest> graceful CLOSE halts (no restart)")
{
  // [CLOSE "a"]
  std::string_view frame = "\x08\x01" "a";

  for (int i = 0; i < 2; i++)
  {
    auto outputs = close_test(
      i != 0, frame, frame,
      {
        R(3),
        W(3),
      },
      {}, {}
    );

    REQUIRE(outputs.restarts.size() == 1);
    CHECK(outputs.restarts[0] == 0);
    CHECK(outputs.all_done);
  }
}

TEST_CASE("<Plain,CloseTest> TEXT then CLOSE in one halts (no restart)")
{
  // [TEXT "hi"] [CLOSE "aaa"]
  std::string_view input = "\x01\x02" "hi" "\x08\x03" "aaa";

  std::deque<FdAction> actions = {
    R(input.size()),
    W(4 + 5),
  };

  auto outputs = close_test(false, input, input, actions, {}, {});

  REQUIRE(outputs.restarts.size() == 1);
  CHECK(outputs.restarts[0] == 0);
  CHECK(outputs.all_done);
}

TEST_CASE("<CloseTest> MULTI-CLOSE initiates graceful shutdown (ET test)")
{
  // [MULTI-CLOSE "a"] at once
  reactor_test1_fragmented({"\x42\x01" "a"}, "\x42\x01" "a");
  // [TEXT "hi"] [MULTI-CLOSE "aa"] at once
  reactor_test1_fragmented(
    {"\x01\x02" "hi" "\x42\x02" "aa"}, "\x01\x02" "hi" "\x42\x02" "aa"
  );
  // [TEXT "hi"] [MULTI-CLOSE "aaa"] fragmented (a fragment per frame)
  reactor_test1_fragmented(
    {"\x01\x02" "hi", "\x42\x03" "aaa"}, "\x01\x02" "hi" "\x42\x03" "aaa"
  );
  // [TEXT "hi"] [MULTI-CLOSE "aaa"] fragmented (3 fragments)
  reactor_test1_fragmented(
    {"\x01\x02" "hi", "\x42\x03" "a", "aa"}, "\x01\x02" "hi" "\x42\x03" "aaa"
  );
  // [TEXT "a"] [TEXT "bc"] [MULTI-CLOSE "xyz"] at once
  reactor_test1_fragmented(
    {"\x01\x01" "a" "\x01\x02" "bc" "\x42\x03" "xyz"},
    "\x01\x01" "a" "\x01\x02" "bc" "\x42\x03" "xyz"
  );
  // [TEXT "a"] [TEXT "bc"] [MULTI-CLOSE "xyz"] but interleaved (4 fragments)
  reactor_test1_fragmented(
    {"\x01\x01" "a", "\x01\x02" "b", "c\x42\x03" "x", "yz"},
    "\x01\x01" "a" "\x01\x02" "bc" "\x42\x03" "xyz"
  );
}

TEST_CASE("<CloseTest> MULTI-CLOSE with zero-length payload")
{
  // [MULTI-CLOSE ""]
  constexpr char frame_raw[] = {'\x42', '\x00'};
  std::string_view frame(frame_raw, 2);

  reactor_test1_fragmented({frame}, frame);
}

TEST_CASE("<CloseTest> multi-frame shutdown via MULTI-CLOSE then CLOSE")
{
  // [MULTI-CLOSE "m"] [TEXT "bc"] [CLOSE "z"] in one read
  std::string_view input = "\x42\x01" "m" "\x01\x02" "bc" "\x08\x01" "z";

  // MULTI-CLOSE in on_data -> close
  // TEXT and final CLOSE are handled by on_shutdown
  reactor_test1_fragmented({input}, input);
}

TEST_CASE("<CloseTest> multi-frame shutdown across CloseProtocol")
{
  // [TEXT "hi"] [MULTI-CLOSE "x"] [TEXT "yz"] [CLOSE "q"]
  // fragmented so that TEXT/MULTI-CLOSE drive Protocol -> CloseProtocol,
  // and the rest is consumed in CloseProtocol.
  reactor_test1_fragmented(
    {
      "\x01\x02" "hi", // [TEXT "hi"]
      "\x42\x01" "x",  // [MULTI-CLOSE "x"] -> enter CloseProtocol
      "\x01\x02" "yz", // [TEXT "yz"] in CloseProtocol
      "\x08\x01" "q"   // final [CLOSE "q"] in CloseProtocol
    },
    "\x01\x02" "hi" "\x42\x01" "x" "\x01\x02" "yz" "\x08\x01" "q"
  );
}

TEST_CASE("<CloseTest> extra frames after final CLOSE are drained")
{
  // [MULTI-CLOSE "m"] [TEXT "bc"] [CLOSE "z"] [TEXT "ignored"]
  // last TEXT should be ignored in DrainProtocol.
  std::string_view input =
    "\x42\x01" "m" "\x01\x02" "bc" "\x08\x01" "z" "\x01\x07" "ignored";

  std::string_view expected = "\x42\x01" "m" "\x01\x02" "bc" "\x08\x01" "z";

  reactor_test1_fragmented({input}, expected);
}

TEST_CASE(
  "<Plain,CloseTest> MULTI-CLOSE then CLOSE restarts connection eagerly"
)
{
  // [MULTI-CLOSE "a"] [CLOSE "b"]
  std::string_view input = "\x42\x01" "a" "\x08\x01" "b";

  for (int i = 0; i < 2; i++)
  {
    auto outputs =
      close_test(i != 0, input, input, gen_script({input}), {}, {});

    REQUIRE(outputs.restarts.size() == 1);
    CHECK(outputs.restarts[0] == 0);
    CHECK(outputs.all_done);
  }
}

TEST_CASE("<Plain,CloseTest> TEXT then MULTI-CLOSE then CLOSE restarts once")
{
  // [TEXT "hi"] [MULTI-CLOSE "xy"] [CLOSE "z"]
  std::string_view input = "\x01\x02" "hi" "\x42\x02" "xy" "\x08\x01" "z";

  std::deque<FdAction> actions = {
    // one read for all three frames, one write for all three echoes
    R(input.size()),
    W(2 + 2 + 2 + 2 + 2 + 1),
  };

  auto outputs = close_test(false, input, input, actions, {}, {});

  REQUIRE(outputs.restarts.size() == 1);
  CHECK(outputs.restarts[0] == 0);
  CHECK(outputs.all_done);
}

TEST_CASE(
  "<Plain,CloseTest> invalid opcode during shutdown errors (no restart)"
)
{
  // [MULTI-CLOSE "m"] [?? "x"]
  // MULTI-CLOSE in on_data -> close ~> enter CloseProtocol
  // invalid opcode is seen in on_shutdown and should cause error + no restart.
  std::string_view input = "\x42\x01" "m" "\xFF\x01" "x";

  std::deque<FdAction> actions = {
    R(input.size()),
    W(6),
  };

  auto outputs = close_test(false, input, "", actions, {}, {});

  CHECK(outputs.all_done);
  CHECK(outputs.restarts.size() == 0);
}

TEST_CASE("<Plain,CloseTest> bare MULTI-CLOSE then CLOSE in one read")
{
  // [MULTI-CLOSE "a"] [CLOSE "b"] at once
  std::string_view input = "\x42\x01" "a" "\x08\x01" "b";

  std::deque<FdAction> actions = {R(input.size()), W(input.size())};

  auto outputs = close_test(false, input, input, actions, {}, {});

  CHECK(outputs.all_done);
  REQUIRE(outputs.restarts.size() == 1);
  CHECK(outputs.restarts[0] == 0);
}
