#include <cstdint>
#include <string_view>
#include <tuple>
#include <variant>

#include "manet/reactor/io.hpp"
#include "manet/transport/plain.hpp"

#include "mock/reactor.hpp"

namespace manet
{

namespace transport
{

struct ReadWantWriteTransport
{
  using config_t = std::monostate;

  template <typename Net> struct Endpoint
  {
    net::TestNet::fd_t fd = -1;
    bool first = true;

    static std::optional<Endpoint>
    init(typename Net::fd_t fd, config_t) noexcept
    {
      return Endpoint{fd, true};
    }

    Status read(reactor::RxSink in) noexcept
    {
      // first readable edge: stall read-side on want_write
      if (first)
      {
        first = false;
        return Status::want_write;
      }

      // afterwards: normal read via net::TestNet
      auto buf = in.wbuf();
      int n = net::TestNet::read(fd, buf.data(), buf.size());

      if (n > 0)
      {
        in.wrote(static_cast<std::size_t>(n));
        return Status::ok;
      }

      if (errno == EAGAIN)
        return Status::want_read;
      return Status::close;
    }

    Status write(reactor::TxSource out) noexcept
    {
      auto buf = out.rbuf();
      int n = net::TestNet::write(fd, buf.data(), buf.size());

      if (n > 0)
      {
        out.read(static_cast<std::size_t>(n));
        return Status::ok;
      }

      if (errno == EAGAIN)
        return Status::want_write;
      return Status::error;
    }

    static void destroy() noexcept {}
  };
};

struct WriteWantReadTransport
{
  using config_t = std::monostate;

  template <typename Net> struct Endpoint
  {
    net::TestNet::fd_t fd = -1;

    int read_calls = 0;
    int write_calls = 0;

    static std::optional<Endpoint>
    init(typename Net::fd_t fd, config_t) noexcept
    {
      return Endpoint{fd, 0, 0};
    }

    Status read(reactor::RxSink in) noexcept
    {
      auto buf = in.wbuf();

      // first readable edge: only consume 1 byte, then stop with want_read
      // so rquota + remaining input stay available for a combined edge later.
      if (read_calls++ == 0)
      {
        int n = net::TestNet::read(fd, buf.data(), 1);
        if (n > 0)
        {
          in.wrote(1);
          return Status::want_read;
        }
        if (errno == EAGAIN)
          return Status::want_read;
        return Status::close;
      }

      // subsequent reads are normal
      int n = net::TestNet::read(fd, buf.data(), buf.size());
      if (n > 0)
      {
        in.wrote(static_cast<std::size_t>(n));
        return Status::ok;
      }

      if (errno == EAGAIN)
        return Status::want_read;
      return Status::close;
    }

    Status write(reactor::TxSource out) noexcept
    {
      // first write attempt blocks on want_read (e.g. TLS renegotiation)
      if (write_calls++ == 0)
        return Status::want_read;

      // retry: normal write via net::TestNet
      auto buf = out.rbuf();
      int n = net::TestNet::write(fd, buf.data(), buf.size());

      if (n > 0)
      {
        out.read(static_cast<std::size_t>(n));
        return Status::ok;
      }

      if (errno == EAGAIN)
        return Status::want_write;
      return Status::error;
    }

    void destroy() noexcept {}
  };
};

struct NoProgressWriteTransport
{
  using config_t = std::monostate;

  template <typename Net> struct Endpoint
  {
    net::TestNet::fd_t fd = -1;
    int write_calls = 0;

    static std::optional<Endpoint>
    init(typename Net::fd_t fd, config_t) noexcept
    {
      return Endpoint{fd, 0};
    }

    Status handshake_step() noexcept
    {
      return Status::ok; // no real handshake
    }

    Status read(reactor::RxSink in) noexcept
    {
      auto buf = in.wbuf();
      int n = net::TestNet::read(fd, buf.data(), buf.size());

      if (n > 0)
      {
        in.wrote(static_cast<std::size_t>(n));
        return Status::ok;
      }

      if (errno == EAGAIN)
        return Status::want_read;

      return Status::close;
    }

    Status write(reactor::TxSource out) noexcept
    {
      // first call: return ok but consume 0 bytes
      if (write_calls++ == 0)
      {
        // no out.read(), no net::TestNet::write -> no progress
        return Status::ok;
      }

      // subsequent calls: normal write via net::TestNet
      auto buf = out.rbuf();
      int n = net::TestNet::write(fd, buf.data(), buf.size());

      if (n > 0)
      {
        out.read(static_cast<std::size_t>(n));
        return Status::ok;
      }

      if (errno == EAGAIN)
        return Status::want_write;

      return Status::error;
    }

    void destroy() noexcept {}
  };
};

struct InitFailTransport
{
  using config_t = std::monostate;

  template <typename Net> struct Endpoint
  {
    static std::optional<Endpoint> init(typename Net::fd_t, config_t) noexcept
    {
      return std::nullopt; // simulate transport init failure
    }

    Status read(reactor::RxSink) noexcept
    {
      return Status::error; // never reached
    }

    Status write(reactor::TxSource) noexcept
    {
      return Status::error; // never reached
    }

    void destroy() noexcept {}
  };
};

struct DrainWantWriteTransport
{
  using config_t = std::monostate;

  template <typename Net> struct Endpoint
  {
    net::TestNet::fd_t fd = -1;
    int write_calls = 0;

    static std::optional<Endpoint>
    init(typename Net::fd_t fd, config_t) noexcept
    {
      return Endpoint{fd, 0};
    }

    Status read(reactor::RxSink in) noexcept
    {
      auto buf = in.wbuf();
      int n = net::TestNet::read(fd, buf.data(), buf.size());

      if (n > 0)
      {
        in.wrote(static_cast<std::size_t>(n));
        return Status::ok;
      }

      if (errno == EAGAIN)
        return Status::want_read;
      return Status::close;
    }

    Status write(reactor::TxSource out) noexcept
    {
      // call0: want_write (from bind_protocol close)
      // call1: want_write (from DrainProtocol transport_write(false))
      // call2+: ok + consume
      if (write_calls++ < 2)
        return Status::want_write;

      auto buf = out.rbuf();
      int n = net::TestNet::write(fd, buf.data(), buf.size());

      if (n > 0)
      {
        out.read(static_cast<std::size_t>(n));
        return Status::ok;
      }

      if (errno == EAGAIN)
        return Status::want_write;
      return Status::error;
    }

    void destroy() noexcept {}
  };
};

struct DrainCloseTransport
{
  using config_t = std::monostate;

  template <typename Net> struct Endpoint
  {
    net::TestNet::fd_t fd = -1;
    int write_calls = 0;

    static std::optional<Endpoint>
    init(typename Net::fd_t fd, config_t) noexcept
    {
      return Endpoint{fd, 0};
    }

    Status read(reactor::RxSink in) noexcept
    {
      auto buf = in.wbuf();
      int n = net::TestNet::read(fd, buf.data(), buf.size());

      if (n > 0)
      {
        in.wrote(static_cast<std::size_t>(n));
        return Status::ok;
      }

      if (errno == EAGAIN)
        return Status::want_read;
      return Status::close;
    }

    Status write(reactor::TxSource out) noexcept
    {
      // call0: want_write (forces DrainProtocol)
      if (write_calls++ == 0)
        return Status::want_write;

      // call1: flush anything pending, then close
      auto buf = out.rbuf();
      int n = net::TestNet::write(fd, buf.data(), buf.size());
      if (n > 0)
        out.read(static_cast<std::size_t>(n));

      return Status::close; // treated as graceful close
    }

    void destroy() noexcept {}
  };
};

} // namespace transport

namespace protocol
{

struct ReflectProtocol
{
  using config_t = std::monostate;

  struct Session
  {
    Session(std::string_view, uint16_t, config_t) noexcept {}

    Status on_data(reactor::IO io) noexcept
    {
      auto in = io.rbuf();
      auto out = io.wbuf();

      if (in.empty())
        return Status::ok;

      if (out.size() < in.size())
        return Status::error;

      std::memcpy(out.data(), in.data(), in.size());
      io.read(in.size());
      io.wrote(in.size());

      return Status::ok;
    }
  };
};

struct CloseAfterWriteProtocol
{
  using config_t = std::monostate;

  struct Session
  {
    Session(std::string_view, uint16_t, config_t) noexcept {}

    Status on_data(reactor::IO io) noexcept
    {
      auto in = io.rbuf();
      auto out = io.wbuf();

      if (in.empty())
        return Status::ok;

      if (out.size() < in.size())
        return Status::error;

      std::memcpy(out.data(), in.data(), in.size());
      io.read(in.size());
      io.wrote(in.size());

      return Status::close; // no on_shutdown -> DrainProtocol branch
    }
  };
};

struct CloseNoShutdownProtocol
{
  using config_t = std::monostate;

  struct Session
  {
    Session(std::string_view, uint16_t, config_t) noexcept {}

    Status on_data(reactor::IO io) noexcept
    {
      auto in = io.rbuf();
      auto out = io.wbuf();

      if (in.empty())
        return Status::ok;

      if (out.size() < in.size())
        return Status::error;

      std::memcpy(out.data(), in.data(), in.size());
      io.read(in.size());
      io.wrote(in.size());

      // no on_shutdown, so this triggers the else-branch in bind_protocol
      return Status::close;
    }
  };
};

} // namespace protocol
} // namespace manet

namespace manet::transport_tests
{

auto R = manet::net::test::FdAction::GrantRead;
auto W = manet::net::test::FdAction::GrantWrite;

template <typename Protocol = protocol::ReflectProtocol>
reactor::test::ReactorOutputs scripted_test(
  bool connect_async, std::initializer_list<std::string_view> frags,
  std::string_view expected_output, std::deque<net::test::FdAction> actions,
  std::initializer_list<transport::Status> handshake_override = {},
  std::initializer_list<transport::Status> read_status_override = {},
  std::initializer_list<transport::Status> write_status_override = {},
  std::initializer_list<transport::Status> shutdown_override = {}
)
{
  std::string input;
  for (auto f : frags)
    input.append(f);

  auto script = transport::happypath(
    frags, handshake_override, write_status_override, shutdown_override
  );

  if (read_status_override.size() != 0)
  {
    script.read_status.clear();
    for (auto st : read_status_override)
      script.read_status.push_back(st);
  }

  if (write_status_override.size() != 0)
  {
    script.write_status.clear();
    for (auto st : write_status_override)
      script.write_status.push_back(st);
  }

  return reactor::test::test1<transport::ScriptedTransport, Protocol>(
    connect_async, input, expected_output, std::move(actions), &script
  );
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> EOF triggers CloseTransport -> Closed"
)
{
  // "hello" then EOF
  std::initializer_list<std::string_view> fragments = {"hello"};
  auto actions = net::gen_script(fragments);

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "hello", actions
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 1); // Closed -> reactor records restart
    CHECK(out.restarts[0] == 0);
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> want_read pauses and resumes read loop"
)
{
  // "a", want_read, "b", then EOF
  std::initializer_list<std::string_view> fragments = {"a", "b"};
  auto actions = net::gen_script(fragments);

  // statuses: ok (read "a"), want_read (no bytes), ok (read "b"), close (EOF)
  std::initializer_list<transport::Status> read_status = {
    transport::Status::ok,
    transport::Status::want_read,
    transport::Status::ok,
    transport::Status::close,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "ab", actions,
      /*handshake*/ {}, /*read_override*/ read_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 1);
    CHECK(out.restarts[0] == 0);
  }
}

TEST_CASE(
  "<ReadWantWriteTransport,ReflectProtocol> want_write from read arms and " "re" "su" "me" "s"
)
{
  // first read returns want_write; next edge with W-quota unblocks,
  // delivering data and echoing it.
  std::string_view input = "ab";

  std::deque<net::test::FdAction> actions = {
    R(2), // first readable edge -> want_write
    W(2), // next poll: readable+writeable edge together
  };

  auto out = reactor::test::test1<
    transport::ReadWantWriteTransport, protocol::ReflectProtocol>(
    /*connect_async*/ false, input,
    /*expected*/ input, actions
  );

  // output asserted by reactor::test::test1
  CHECK(out.restarts.size() == 0);
}

TEST_CASE(
  "<WriteWantReadTransport,ReflectProtocol> want_read from write arms and " "re" "tr" "ie" "s " "af" "te" "r " "re" "ad" "ab" "le" " e" "dg" "e"
)
{
  // read 1 byte -> echo -> write wants read; next combined edge retries write.
  std::string_view input = "xy";

  std::deque<net::test::FdAction> actions = {
    R(2), // readable edge; read consumes 1 byte then want_read
    W(
      2
    ), // next poll: readable+writeable edge (rquota still 1, input still "y")
  };

  auto out = reactor::test::test1<
    transport::WriteWantReadTransport, protocol::ReflectProtocol>(
    /*connect_async*/ false, input,
    /*expected*/ input, actions
  );

  CHECK(out.restarts.size() == 0);
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> want_write retries on next writeable " "edge"
)
{
  // "xx" but transport blocks first write
  std::initializer_list<std::string_view> fragments = {"xx"};
  std::deque<net::test::FdAction> actions = {R(2), W(2), W(2)};

  // prevent happypath EOF in the same readable event:
  // ok (deliver "xx"), then want_read (pause), no close here
  std::initializer_list<transport::Status> read_status = {
    transport::Status::ok,
    transport::Status::want_read,
  };

  // first write attempt -> want_write, second -> ok
  std::initializer_list<transport::Status> write_status = {
    transport::Status::want_write,
    transport::Status::ok,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "xx", actions,
      /*handshake*/ {},
      /*read_override*/ read_status,
      /*write_override*/ write_status
    );

    auto restart = out.restarts.size() == 1 && out.restarts[0] == 0;
    CHECK_MESSAGE(restart, "restart should set");
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> write close enters CloseTransport " "imm" "edi" "ate" "ly"
)
{
  // "yy" but transport closes during first write
  std::initializer_list<std::string_view> fragments = {"yy"};

  // we only need a read edge; close happens during write from readable path
  std::deque<net::test::FdAction> actions = {R(2)};

  std::initializer_list<transport::Status> write_status = {
    transport::Status::close,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, /*expected*/ "", actions,
      /*handshake*/ {}, /*read_override*/ {}, /*write_override*/ write_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 1); // Closed, not Error
    CHECK(out.restarts[0] == 0);
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> read error transitions to Error (no " "r" "e" "s" "t" "a" "r" "t" ")"
)
{
  // transport errors before delivering any bytes
  std::initializer_list<std::string_view> fragments = {"zz"};

  std::deque<net::test::FdAction> actions = {R(2)};

  std::initializer_list<transport::Status> read_status = {
    transport::Status::error,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, /*expected*/ "", actions,
      /*handshake*/ {}, /*read_override*/ read_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 0); // Error does not count as closed()
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> handshake ok transitions to Protocol"
)
{
  // "hi" after handshake ok
  std::initializer_list<std::string_view> fragments = {"hi"};
  auto actions = net::gen_script(fragments);

  std::initializer_list<transport::Status> handshake_status = {
    transport::Status::ok,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "hi", actions,
      /*handshake*/ handshake_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 1);
    CHECK(out.restarts[0] == 0);
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> handshake want_read pauses then resumes"
)
{
  // want_read, then ok; same readable edge also drives first protocol read
  std::initializer_list<std::string_view> fragments = {"x"};
  auto actions = net::gen_script(fragments);

  std::initializer_list<transport::Status> handshake_status = {
    transport::Status::want_read,
    transport::Status::ok,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "x", actions,
      /*handshake*/ handshake_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 1);
    CHECK(out.restarts[0] == 0);
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> handshake want_write pauses then resumes"
)
{
  // want_write, then ok; writeable edge unblocks handshake
  std::initializer_list<std::string_view> fragments = {"hi"};

  std::deque<net::test::FdAction> actions = {
    W(1), // handshake want_write edge
    R(2),
    W(2),
  };

  std::initializer_list<transport::Status> handshake_status = {
    transport::Status::want_write,
    transport::Status::ok,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "hi", actions,
      /*handshake*/ handshake_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 1);
    CHECK(out.restarts[0] == 0);
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> handshake close errors (no restart)"
)
{
  // handshake fails eagerly
  std::initializer_list<std::string_view> fragments = {};
  std::deque<net::test::FdAction> actions = {};

  std::initializer_list<transport::Status> handshake_status = {
    transport::Status::close,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "", actions,
      /*handshake*/ handshake_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 0);
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> handshake error errors (no restart)"
)
{
  // handshake fails eagerly
  std::initializer_list<std::string_view> fragments = {};
  std::deque<net::test::FdAction> actions = {};

  std::initializer_list<transport::Status> handshake_status = {
    transport::Status::error,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "", actions,
      /*handshake*/ handshake_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 0);
  }
}

TEST_CASE("<ScriptedTransport,ReflectProtocol> shutdown ok closes cleanly")
{
  // "ok" then EOF -> CloseTransport -> shutdown ok -> Closed
  std::initializer_list<std::string_view> fragments = {"ok"};
  auto actions = net::gen_script(fragments);

  std::initializer_list<transport::Status> shutdown_status = {
    transport::Status::ok,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "ok", actions,
      /*handshake*/ {}, /*read_override*/ {}, /*write_override*/ {},
      /*shutdown*/ shutdown_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 1);
    CHECK(out.restarts[0] == 0);
  }
}

TEST_CASE("<ScriptedTransport,ReflectProtocol> shutdown want_write then ok")
{
  // EOF enters CloseTransport; shutdown blocks on want_write, then ok on
  // writeable edge
  std::initializer_list<std::string_view> fragments = {"hi"};

  std::deque<net::test::FdAction> actions = {
    R(2),
    W(1), // writeable edge for shutdown want_write
  };

  std::initializer_list<transport::Status> shutdown_status = {
    transport::Status::want_write,
    transport::Status::ok,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "hi", actions,
      /*handshake*/ {}, /*read_override*/ {}, /*write_override*/ {},
      /*shutdown*/ shutdown_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 1);
    CHECK(out.restarts[0] == 0);
  }
}

TEST_CASE("<ScriptedTransport,ReflectProtocol> shutdown want_read then ok")
{
  // EOF enters CloseTransport; shutdown blocks on want_read, then ok on
  // readable edge
  std::initializer_list<std::string_view> fragments = {"hi"};

  std::deque<net::test::FdAction> actions = {
    R(2),
    R(1), // readable edge for shutdown want_read
  };

  std::initializer_list<transport::Status> shutdown_status = {
    transport::Status::want_read,
    transport::Status::ok,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "hi", actions,
      /*handshake*/ {}, /*read_override*/ {}, /*write_override*/ {},
      /*shutdown*/ shutdown_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 1);
    CHECK(out.restarts[0] == 0);
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> shutdown close errors (no restart)"
)
{
  // EOF enters CloseTransport; shutdown close -> Error
  std::initializer_list<std::string_view> fragments = {"x"};
  auto actions = net::gen_script(fragments);

  std::initializer_list<transport::Status> shutdown_status = {
    transport::Status::close,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "x", actions,
      /*handshake*/ {}, /*read_override*/ {}, /*write_override*/ {},
      /*shutdown*/ shutdown_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 0);
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> shutdown error errors (no restart)"
)
{
  // EOF enters CloseTransport; shutdown error -> Error
  std::initializer_list<std::string_view> fragments = {"x"};
  auto actions = net::gen_script(fragments);

  std::initializer_list<transport::Status> shutdown_status = {
    transport::Status::error,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "x", actions,
      /*handshake*/ {}, /*read_override*/ {}, /*write_override*/ {},
      /*shutdown*/ shutdown_status
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 0);
  }
}

TEST_CASE("<ScriptedTransport,ReflectProtocol> handshake ok enters Protocol")
{
  // no payload, single handshake_step = ok
  std::initializer_list<std::string_view> fragments = {};
  std::deque<net::test::FdAction> actions = {};

  std::initializer_list<transport::Status> handshake = {
    transport::Status::ok,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, /*expected*/ "", actions,
      /*handshake*/ handshake
    );

    // Handshake should succeed and not error / restart.
    CHECK(out.restarts.size() == 0);
  }
}

TEST_CASE("<ScriptedTransport,ReflectProtocol> handshake want_read then ok")
{
  // no payload, handshake: want_read, then ok
  std::initializer_list<std::string_view> fragments = {};
  std::deque<net::test::FdAction> actions = {};

  std::initializer_list<transport::Status> handshake = {
    transport::Status::want_read,
    transport::Status::ok,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, /*expected*/ "", actions,
      /*handshake*/ handshake
    );

    CHECK(out.restarts.size() == 0);
  }
}

TEST_CASE("<ScriptedTransport,ReflectProtocol> handshake want_write then ok")
{
  // no payload, handshake: want_write, then ok
  std::initializer_list<std::string_view> fragments = {};
  std::deque<net::test::FdAction> actions = {};

  std::initializer_list<transport::Status> handshake = {
    transport::Status::want_write,
    transport::Status::ok,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, /*expected*/ "", actions,
      /*handshake*/ handshake
    );

    CHECK(out.restarts.size() == 0);
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> handshake close transitions to Error"
)
{
  // handshake_step returns close immediately
  std::initializer_list<std::string_view> fragments = {};
  std::deque<net::test::FdAction> actions = {};

  std::initializer_list<transport::Status> handshake = {
    transport::Status::close,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, /*expected*/ "", actions,
      /*handshake*/ handshake
    );

    CHECK(out.all_done);             // Connection ended
    CHECK(out.restarts.size() == 0); // Error, not Closed
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> handshake error transitions to Error"
)
{
  // handshake_step returns error immediately
  std::initializer_list<std::string_view> fragments = {};
  std::deque<net::test::FdAction> actions = {};

  std::initializer_list<transport::Status> handshake = {
    transport::Status::error,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, /*expected*/ "", actions,
      /*handshake*/ handshake
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 0);
  }
}

TEST_CASE(
  "<ScriptedTransport,ReflectProtocol> shutdown close transitions to Error"
)
{
  // "hello" then EOF; transport shutdown_step returns close
  std::initializer_list<std::string_view> fragments = {"hello"};
  auto actions = net::gen_script(fragments);

  std::initializer_list<transport::Status> shutdown = {
    transport::Status::close,
  };

  for (int i = 0; i < 2; i++)
  {
    auto out = scripted_test<protocol::ReflectProtocol>(
      i != 0, fragments, "hello", actions,
      /*handshake*/ {},
      /*read_override*/ {},
      /*write_override*/ {},
      /*shutdown_override*/ shutdown
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 0); // Error, not Closed -> no restart
  }
}

TEST_CASE(
  "<NoProgressWriteTransport,ReflectProtocol> ok-but-no-progress arms " "want_" "write"
)
{
  // single byte "x", first write ok-but-no-progress,
  // second writeable edge flushes.
  std::string_view input = "x";
  std::initializer_list<std::string_view> fragments = {input};

  std::deque<net::test::FdAction> actions = {
    R(input.size()),
    W(1), // triggers ok-but-no-progress -> arm want_write
    W(1), // retry flushes
  };

  auto outputs = reactor::test::test1<
    transport::NoProgressWriteTransport, protocol::ReflectProtocol>(
    /*connect_async*/ false, input,
    /*expected_output*/ input, actions
  );

  // We can't require all_done because Protocol ignores close-only HUP edges.
  CHECK(outputs.restarts.size() == 0);
}

TEST_CASE(
  "<DrainWantWriteTransport,CloseAfterWriteProtocol> DrainProtocol want_write " "then ok"
)
{
  // "xy" echoed and then protocol returns close without on_shutdown,
  // forcing the !HasProtocolShutdown branch:
  //   - first transport_write: want_write -> enter DrainProtocol
  //   - in DrainProtocol, transport_write(false) returns want_write again
  //   - finally transport_write(false) drains TX and returns ok ->
  //   CloseTransport->Closed
  std::string_view input = "xy";
  std::initializer_list<std::string_view> fragments = {input};

  // One read event, and enough writeable edges for:
  //   - close-branch transport_write
  //   - DrainProtocol write want_write
  //   - DrainProtocol final ok write
  std::deque<net::test::FdAction> actions = {
    R(input.size()),
    W(input.size()),
    W(input.size()),
    W(input.size()),
  };

  auto outputs = reactor::test::test1<
    transport::DrainWantWriteTransport, protocol::CloseAfterWriteProtocol>(
    /*connect_async*/ false, input,
    /*expected_output*/ input, actions
  );

  CHECK(outputs.all_done);
  CHECK(outputs.restarts.size() == 1);
  CHECK(outputs.restarts[0] == 0); // Closed, not Error
}

TEST_CASE(
  "<InitFailTransport,ReflectProtocol> init failure transitions to Error (no " "restart)"
)
{
  // transport init returns nullopt during enter_connected()
  std::string_view input = "";

  for (int i = 0; i < 2; i++)
  {
    // async connect needs a writeable edge to finish connect
    std::deque<net::test::FdAction> actions =
      (i != 0) ? std::deque<net::test::FdAction>{W(1)}
               : std::deque<net::test::FdAction>{};

    auto out = reactor::test::test1<
      transport::InitFailTransport, protocol::ReflectProtocol>(
      i != 0, input, /*expected*/ "", actions
    );

    CHECK(out.all_done);
    CHECK(out.restarts.size() == 0); // Error, not Closed
  }
}

TEST_CASE(
  "<Plain,CloseNoShutdownProtocol> close with limited writes still halts " "cle" "anl" "y"
)
{
  // "abcd" with limited write quota to force multiple writes,
  // i.e. we expect to go through the DrainProtocol path internally,
  // but from the outside we only assert: echo is correct and we finish.
  std::string_view input = "abcd";

  std::deque<net::test::FdAction> actions = {
    R(input.size()),
    W(1), // partial write, then EAGAIN -> want_write
    W(2), // more bytes
    W(1), // drain remaining byte
  };

  auto outputs =
    reactor::test::test1<transport::Plain, protocol::CloseNoShutdownProtocol>(
      /*connect_async*/ false, input,
      /*expected_output*/ input, actions
    );

  CHECK(outputs.all_done);
  CHECK(outputs.restarts.size() == 1);
  CHECK(outputs.restarts[0] == 0);
};

TEST_CASE(
  "<Plain,CloseNoShutdownProtocol> close with drained TX goes directly to " "Cl" "os" "eT" "ra" "ns" "po" "rt"
)
{
  // single frame "hi" echoed once, TX drains in one go,
  // so bind_protocol(close) sees transport_write() == true and enters
  // CloseTransport.
  std::string_view input = "hi";

  std::deque<net::test::FdAction> actions = {
    R(input.size()),
    W(input.size()),
  };

  auto outputs =
    reactor::test::test1<transport::Plain, protocol::CloseNoShutdownProtocol>(
      /*connect_async*/ false, input,
      /*expected_output*/ input, actions
    );

  CHECK(outputs.all_done);
  CHECK(outputs.restarts.size() == 1);
  CHECK(outputs.restarts[0] == 0); // Closed
}

} // namespace manet::transport_tests
