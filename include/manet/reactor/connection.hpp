#pragma once

#include <cstring>
#include <string>

#include "manet/net/concepts.hpp"
#include "manet/net/dial.hpp"
#include "manet/protocol/concepts.hpp"
#include "manet/transport/concepts.hpp"

#include "manet/logging.hpp"

namespace manet::reactor
{

/**
 * Type-erased interface for Connection.
 *
 * Hide concrete <Transport, Protocol> types so a reactor can manage
 * heterogeneous connections (see Reactor or TestReactor)
 *
 * @tparam Net the network implementation (for example POSIX or F-Stack).
 *         Must satisfy Net.
 */
template <typename Net>
  requires net::Net<Net>
struct BaseConnection
{
  virtual void handle_event(typename Net::event_t &) noexcept = 0;
  virtual void restart() noexcept = 0;
  virtual bool closed() const noexcept = 0;
  virtual bool done() const noexcept = 0;

  virtual ~BaseConnection() = default;
};

/**
 * Generic Connection<Net, Transport, Protocol>; edge-triggered,
 * asynchronous, non-blocking connection state machine for layers:
 *
 * - network: FDs and event demultiplexing
 *
 * - transport: IO and (optional) handshake/shutdown
 *
 * - protocol: frame handling and (optional) heartbeat/shutdown.
 *
 * #### NOTE
 *
 * - `attach(cookie)` must be called once before `handle_event`
 *
 * - `restart()` only takes effect when `done()`
 *
 * #### Machine states:
 *
 * - uninitialized (transient): reset, dial non-blocking FD, initialize
 * protocol.
 *
 * - in_progress: wait for writeable event (for asynchronous connect)
 *
 * - Transport: asynchronous handshake (if declared)
 *
 * - Protocol (normal operation): Reads drain RX fully and feed protocol frames
 * until exhausted, Writes drain TX fully.
 *
 * - close_protocol: graceful protocol shutdown while still reading (keep
 * calling on_shutdown until Close)
 *
 * - drain_protocol: protocol finished; empty RX, flush TX and then close
 * transport.
 *
 * - close_transport: graceful transport shutdown (if declared), then
 * closed/error.
 *
 * @tparam Net the network implementation (for example POSIX or F-Stack).
 *         Must satisfy Net.
 * @tparam Transport the transport layer for this connection.
 *         Must satisfy Transport.
 * @tparam Protocol the Protocol layer for this connection (Session,
 * Presentation, Application). Must satisfy Protocol.
 *
 */
template <typename Net, typename Transport, typename Protocol>
  requires net::Net<Net> && transport::Transport<Net, Transport> &&
           protocol::Protocol<Protocol>
class Connection : public BaseConnection<Net>
{
public:
  using protocol_t = Protocol;

  using Endpoint = typename Transport::template Endpoint<Net>;
  using Session = typename Protocol::Session;

  Connection(
    const std::string &host, uint16_t port,
    typename Transport::config_t transport_config,
    typename Protocol::config_t protocol_config
  )
      : _protocol(Session{host, port, protocol_config}),
        _transport_config(std::move(transport_config)),
        _protocol_config(std::move(protocol_config)),
        _host(host),
        _fd(-1),
        _state(state_t::uninitialized),
        _port(port)
  {
    static_assert(std::is_nothrow_move_assignable_v<Endpoint>);
    static_assert(std::is_nothrow_move_constructible_v<Endpoint>);
    static_assert(std::is_nothrow_destructible_v<Endpoint>);

    static_assert(std::is_trivially_destructible_v<Buffer<RX_CAP>>);
    static_assert(std::is_trivially_destructible_v<Buffer<TX_CAP>>);
  }

  void attach(void *cookie) noexcept
  {
    if (_cookie != nullptr)
    {
      log::error("already attached ({} {})", _fd, _cookie);
      return;
    }

    _cookie = cookie;

    enter_uninitialized(); // kick off the connection
  }

  /** steps the FSM (runs until state stabilizes or no progress)
   *
   * @param[in] ev the new edge event
   */
  void handle_event(typename Net::event_t &ev) noexcept override
  {
    if constexpr (log::enabled)
    {
      log::trace(
        "Connection::handle_event({}, {}, {} {} {} {} {})", _fd,
        to_string(_state), Net::ev_signal(ev) ? "S" : "-",
        Net::ev_close(ev) ? "C" : "-", Net::ev_error(ev) ? "E" : "-",
        Net::ev_readable(ev) ? "R" : "-", Net::ev_writeable(ev) ? "W" : "-"
      );
    }

    steps(&ev);
  }

  void heartbeat() noexcept
  {
    if constexpr (protocol::HasHeartbeat<Protocol>)
    {
      if (_state == state_t::protocol)
      {
        _protocol.heartbeat(Output{&_tx});
        transport_write();
      }
    }
  }

  void restart() noexcept override
  {
    if (!done())
      return;

    teardown();

    _protocol = Session{_host, _port, _protocol_config};

    enter_uninitialized();
  }

  /** triggers graceful shutdown. */
  void stop() noexcept
  {
    switch (_state)
    {
    case state_t::uninitialized:
    case state_t::in_progress:
      enter_closed();
      break;
    case state_t::transport:
      enter_close_transport();
      break;
    case state_t::protocol:
      enter_close_protocol();
      break;
    default:
      break;
    }

    steps(nullptr);
  }

  bool closed() const noexcept override { return _state == state_t::closed; }

  bool done() const noexcept override
  {
    return _state == state_t::error || _state == state_t::closed;
  }

  ~Connection() override { teardown(); }

  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;
  Connection(Connection &&) = delete;
  Connection &operator=(Connection &&) = delete;

private:
  enum class state_t : uint8_t
  {
    uninitialized,
    in_progress,
    transport,
    protocol,
    close_protocol,
    drain_protocol,
    close_transport,
    closed,
    error
  };

  static constexpr std::string_view to_string(state_t state)
  {
    switch (state)
    {
    case state_t::uninitialized:
      return "uninitialized";
    case state_t::in_progress:
      return "in_progress";
    case state_t::transport:
      return "transport";
    case state_t::protocol:
      return "Protocol";
    case state_t::close_protocol:
      return "close_protocol";
    case state_t::drain_protocol:
      return "drain_protocol";
    case state_t::close_transport:
      return "close_transport";
    case state_t::closed:
      return "closed";
    case state_t::error:
      return "error";

    default:
      return "~";
    }
  }

  Buffer<RX_CAP> _rx;
  Buffer<TX_CAP> _tx;

  Endpoint _transport;
  Session _protocol;

  typename Transport::config_t _transport_config;
  typename Protocol::config_t _protocol_config;

  const std::string _host;
  void *_cookie = nullptr;

  typename Net::fd_t _fd;
  state_t _state;
  uint16_t _port;

  void steps(typename Net::event_t *ev) noexcept
  {
    while (true)
    {
      state_t before = _state;

      switch (before)
      {
      case state_t::uninitialized:
        std::unreachable();
      case state_t::in_progress:
      {
        if (!ev)
          return;
        step_in_progress(*ev);
        break;
      }
      case state_t::transport:
      {
        step_Transport();
        break;
      }
      case state_t::protocol:
      {
        if (!ev)
          return;
        step_Protocol(*ev);
        break;
      }
      case state_t::close_protocol:
      {
        if (!ev)
          return;
        step_close_protocol(*ev);
        break;
      }
      case state_t::drain_protocol:
      {
        step_drain_protocol(ev);
        break;
      }
      case state_t::close_transport:
      {
        step_close_transport();
        break;
      }
      case state_t::closed:
      case state_t::error:
        return;
      }

      if (_state == before)
      {
        return;
      }
    }
  }

  void enter_uninitialized()
  {
    _state = state_t::uninitialized;

    _rx.clear();
    _tx.clear();

    net::DialResult<Net> result = net::dial<Net>(_host.c_str(), _port);

    if (result.fd == -1)
    {
      log::error(
        "dial({}, {}) failed: {}", _host, _port, std::strerror(result.err)
      );

      _fd = -1;
      _state = state_t::error;
      return;
    }

    _fd = result.fd;

    if (result.err == EINPROGRESS)
    {
      // next event will be writeable
      _state = state_t::in_progress;
      Net::subscribe(_cookie, _fd, false, true);
    }
    else
    {
      // for some reason dial was synchronous. we kick it off:
      enter_connected();
      steps(nullptr);
    }
  }

  void enter_connected() noexcept
  {
    auto transport = Endpoint::init(_fd, _transport_config);
    if (!transport.has_value())
    {
      // enter_error (but do not teardown Transport):
      _state = state_t::error;

      if (_fd != -1)
      {
        if constexpr (protocol::HasTeardown<Protocol>)
        {
          _protocol.teardown();
        }

        Net::clear(_fd);
        Net::close(_fd);

        _fd = -1;
      }

      return;
    }

    _transport = std::move(*transport);

    if constexpr (transport::HasHandshake<Net, Transport>)
    {
      // let step_Transport continue
      _state = state_t::transport;
    }
    else
    {
      // transport "handshake"
      Net::subscribe(_cookie, _fd, true, false);

      // move to protocol
      enter_Protocol();
    }
  }

  void enter_Protocol() noexcept
  {
    _state = state_t::protocol;

    if constexpr (protocol::HasConnectHandler<Protocol>)
    {
      bind_protocol<&Session::on_connect>();
    }
    else
    {
      transport_write();
    }
  }

  void enter_close_protocol() noexcept
  {
    if constexpr (protocol::HasShutdown<Protocol>)
    {
      _state = state_t::close_protocol;

      while (true)
      {
        auto before = _rx.rbuf().size();

        switch (_protocol.on_shutdown(IO{Input{&_rx}, Output{&_tx}}))
        {
        case protocol::Status::ok:
        {
          transport_write();
          if (_state != state_t::close_protocol)
          {
            return;
          } // done/error
          if (before <= _rx.rbuf().size())
          {
            return;
          } // no progress
          break;
        }
        case protocol::Status::close:
        {
          // protocol shutdown done: drain tx
          _state = state_t::drain_protocol;
          return;
        }
        case protocol::Status::error:
        {
          enter_error();
          return;
        }
        }
      }
    }
    else
    {
      enter_close_transport();
    }
  }

  void enter_close_transport() noexcept { _state = state_t::close_transport; }

  void enter_error() noexcept
  {
    _state = state_t::error;
    teardown();
  }

  void enter_closed() noexcept
  {
    _state = state_t::closed;
    teardown();
  }

  void step_in_progress(typename Net::event_t &ev) noexcept
  {
    if (Net::ev_writeable(ev))
    {
      int err = 0;
      socklen_t elen = sizeof(err);

      if (Net::getsockopt(_fd, SOL_SOCKET, SO_ERROR, &err, &elen) == -1)
      {
        int e = errno;
        log::error(
          "getsockopt({}) failed (host={}, errno={} {})", _fd, _host, e,
          std::strerror(e)
        );
        enter_error();
      }
      else if (err != 0)
      {
        log::error(
          "getsockopt({}) failed (host={}, error={} {})", _fd, _host, err,
          std::strerror(err)
        );
        enter_error();
      }
      else
      {
        log::info("connected to {}:{} ({})", _host, _port, _fd);
        enter_connected();
      }
    }
    else if (Net::ev_error(ev) || Net::ev_close(ev))
    {
      enter_error();
    }
  }

  void step_Transport() noexcept
  {
    // just to keep the compiler happy (condition always true otw. Transport
    // state is skipped)
    if constexpr (transport::HasHandshake<Net, Transport>)
    {
      transport::Status status = _transport.handshake_step();
      if (status == transport::Status::ok)
      {
        enter_Protocol();
      }
      else if (status == transport::Status::close)
      {
        enter_error();
      }
      else
      {
        arm(status);
      }
    }
  }

  void protocol_consume() noexcept
  {
    while (true)
    {
      // attempt reading a frame:
      auto before = _rx.rbuf().size();
      if (before == 0)
      {
        return;
      }

      bind_protocol<&Session::on_data>();

      // protocol layer changed state -> done
      if (_state != state_t::protocol)
      {
        return;
      }

      // no progress -> done
      if (before <= _rx.rbuf().size())
      {
        return;
      }
    }
  }

  void step_Protocol(typename Net::event_t &ev) noexcept
  {
    if (Net::ev_readable(ev))
    {
      // keep reading form Transport while protocol stays in Protocol state
      transport_read(
        [this]()
        {
          protocol_consume();
          return _state == state_t::protocol;
        },
        [this]() { enter_close_transport(); }
      );
    }

    if (Net::ev_writeable(ev))
    {
      transport_write();
    }
  }

  void step_close_protocol(typename Net::event_t &ev) noexcept
  {
    if (Net::ev_readable(ev))
    {
      // keep reading from Transport while on_shutdown consumes frames and stays
      // in close_protocol state
      transport_read(
        [this]()
        {
          if constexpr (protocol::HasShutdown<Protocol>)
          {
            // call `on_shutdown` repeatedly in case of multiple frames
            while (true)
            {
              auto before = _rx.rbuf().size();

              switch (_protocol.on_shutdown(IO{Input{&_rx}, Output{&_tx}}))
              {
              case protocol::Status::ok:
                transport_write();
                // early done:
                if (_state != state_t::close_protocol)
                {
                  return false;
                }
                // no progress -> arm(..)
                if (before <= _rx.rbuf().size())
                {
                  return true;
                }
                break; // keep consuming
              case protocol::Status::close:
                _state = state_t::drain_protocol;
                return false;
              case protocol::Status::error:
                enter_error();
                return false;
              }
            }
          }

          // fallback (shouldn't happen since !HasProtocolShutdown skips)
          _state = state_t::drain_protocol;
          return false;
        },
        [this]() { enter_close_transport(); }
      );
    }

    if (Net::ev_writeable(ev))
    {
      transport_write();
    }
  }

  void step_drain_protocol(typename Net::event_t *ev) noexcept
  {
    if (ev && Net::ev_readable(*ev))
    {
      // drain rx and ignore incoming data
      transport_read([]() { return true; }, []() {});
      _rx.clear();
    }

    if (transport_write(false))
    {
      enter_close_transport();
    }
  }

  void step_close_transport() noexcept
  {
    if constexpr (transport::HasShutdown<Net, Transport>)
    {
      transport::Status status = _transport.shutdown_step();
      switch (status)
      {
      case transport::Status::ok:
        enter_closed();
        return;
      case transport::Status::error:
      case transport::Status::close:
        enter_error();
        return;
      default:
        arm(status);
        return;
      }
    }
    else
    {
      enter_closed();
    }
  }

  void arm(transport::Status status) noexcept
  {
    switch (status)
    {
    case transport::Status::want_read:
    {
      Net::subscribe(_cookie, _fd, true, !_tx.rbuf().empty());
      break;
    }
    case transport::Status::want_write:
    {
      auto want_read =
        _state == state_t::protocol || _state == state_t::close_protocol;
      Net::subscribe(_cookie, _fd, want_read, true);
      break;
    }
    case transport::Status::error:
    {
      enter_error();
      break;
    }
    // handled by caller
    default:
      break;
    }
  }

  // always called when _state == Protocol
  template <protocol::Status (Session::*Handler)(IO) noexcept>
  void bind_protocol() noexcept
  {
    switch ((_protocol.*Handler)(IO{Input{&_rx}, Output{&_tx}}))
    {
    case protocol::Status::ok:
    {
      transport_write();
      break;
    }
    case protocol::Status::close:
    {
      transport_write();

      if constexpr (protocol::HasShutdown<Protocol>)
      {
        if (_state == state_t::protocol)
        {
          enter_close_protocol();
        }
        else
        {
          enter_close_transport();
        }
      }
      else
      {
        if (transport_write())
        {
          enter_close_transport();
        }
        else
        {
          _state = state_t::drain_protocol;
        }
      }
      break;
    }
    case protocol::Status::error:
    {
      enter_error();
      break;
    }
    }
  }

  void transport_read(auto &&consume, auto &&on_close) noexcept
  {
    while (true)
    {
      if (_rx.full())
      {
        log::trace("rx_buf({}):\n{}", _fd, _rx.hexdump());
        log::error("rx buffer overflow ({} {})", _fd, RX_CAP);
        enter_error();
        return;
      }

      auto before = _rx.rbuf().size();
      transport::Status st = _transport.read(Output{&_rx});
      auto after = _rx.rbuf().size();

      if (after != before)
      {
        if (!consume())
          return; // no progress
      }

      switch (st)
      {
      case transport::Status::ok:
        // keep draining until want_read/close/error
        continue;

      case transport::Status::close:
        on_close();
        return;

      case transport::Status::want_read:
      case transport::Status::want_write:
        arm(st);
        return;

      case transport::Status::error:
      default:
        enter_error();
        return;
      }
    }
  }

  bool transport_write(bool re_arm = true) noexcept
  {
    while (_fd != -1 && !_tx.rbuf().empty())
    {
      auto before = _tx.rbuf().size();
      transport::Status status = _transport.write(Input{&_tx});

      if (status == transport::Status::close && _state != state_t::error)
      {
        enter_close_transport();
        return false;
      }
      else if (status != transport::Status::ok)
      {
        arm(status);
        return false;
      }

      // (a well-behaved Transport will not returns transport::Status::ok
      // without consuming at least 1 byte)
      if (_tx.rbuf().size() == before)
      {
        // no progress was made
        arm(transport::Status::want_write);
        return false;
      }
    }

    if (re_arm && _fd != -1 &&
        (_state == state_t::protocol || _state == state_t::close_protocol))
    {
      Net::subscribe(_cookie, _fd, true, false);
    }

    return true;
  }

  void teardown() noexcept
  {
    if (_fd != -1)
    {
      if constexpr (protocol::HasTeardown<Protocol>)
      {
        _protocol.teardown();
      }

      _transport.destroy();

      Net::clear(_fd);
      Net::close(_fd);

      _fd = -1;
    }
  }
};

} // namespace manet::reactor
