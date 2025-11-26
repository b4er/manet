#pragma once
#include <doctest/doctest.h>

#include "net.hpp"
#include "transport.hpp"

namespace manet::reactor::test
{

template <typename... Connections> class TestReactor
{
public:
  using event_t = typename net::TestNet::event_t;

  static constexpr std::size_t NUM_CONNECTIONS = sizeof...(Connections);
  static constexpr std::size_t NUM_EVENTS = NUM_CONNECTIONS + 1;

  std::tuple<std::optional<Connections>...> connections{};
  std::array<event_t, NUM_EVENTS> events{};

  static inline uint64_t counter = 0;
  bool stopping = false;

  // test outputs
  std::vector<int> restarts = {};

  template <typename... Configs>
  TestReactor(
    net::TestNet::config_t &config, const std::tuple<Configs...> &cfgs
  )
  {
    net::TestNet::init(config);
    init(cfgs, std::make_index_sequence<NUM_CONNECTIONS>{});
    net::TestNet::run(loop, this);
  }

  static std::vector<std::span<const std::byte>> outputs() noexcept
  {
    std::vector<std::span<const std::byte>> res;
    res.reserve(NUM_CONNECTIONS);

    for (int i = 0; i < NUM_CONNECTIONS; i++)
    {
      res.emplace_back(net::TestNet::_output(i));
    }

    return res;
  }

  bool all_done() const noexcept
  {
    return std::apply(
      [](auto const &...opts) { return ((!opts || opts->done()) && ...); },
      connections
    );
  }

private:
  std::unordered_map<void *, std::size_t> _conn_ids = {};

  int _conn_id(void *conn)
  {
    return _conn_ids.contains(conn) ? _conn_ids[conn] : -1;
  }

  template <typename Configs, std::size_t... I>
  void init(const Configs &cfgs, std::index_sequence<I...>)
  {
    (init_connection<I>(std::get<I>(cfgs)), ...);
  }

  template <std::size_t I, typename Config>
  void init_connection(const Config &cfg)
  {
    using Conn = std::tuple_element_t<I, std::tuple<Connections...>>;

    constexpr std::string_view host = "localhost";
    constexpr uint16_t port = 101;

    auto &opt = std::get<I>(connections);
    opt.emplace(host, port, std::get<0>(cfg), std::get<1>(cfg));

    Conn *conn = std::addressof(*opt);
    conn->attach(static_cast<BaseConnection<net::TestNet> *>(conn));

    _conn_ids[conn] = I;
  }

  static int loop(void *data) noexcept
  {
    auto *self = static_cast<TestReactor *>(data);

    int nevents = net::TestNet::poll(self->events.data(), NUM_EVENTS);
    if (nevents < 0)
    {
      utils::error("poll failed");
      net::TestNet::stop();
    }

    for (int i = 0; i < nevents; i++)
    {
      auto &ev = self->events[i];

      auto kill = net::TestNet::ev_signal(ev);
      if (kill)
      {
        if (!self->stopping)
        {
          self->stopping = true;
          self->stop_all();
        }
      }
      else
      {
        void *ptr = net::TestNet::get_user_data(ev);
        auto conn = static_cast<BaseConnection<net::TestNet> *>(ptr);

        if (!conn->done())
        {
          conn->handle_event(ev);

          if (!self->stopping && conn->closed())
          {
            // conn->restart();
            self->restarts.push_back(self->_conn_id(conn));
          }
        }
      }

      if (self->stopping && self->all_done())
      {
        net::TestNet::stop();
      }
    }

    if ((++counter & 127) == 0)
    {
      self->heartbeat();
    }

    return 0;
  }

  void heartbeat() noexcept
  {
    std::apply(
      [](auto &...opt) { ((opt ? opt->heartbeat() : void()), ...); },
      connections
    );
  }

  void stop_all() noexcept
  {
    std::apply(
      [](auto &...opt) { ((opt ? opt->stop() : void()), ...); }, connections
    );
  }
};

struct ReactorOutputs
{
  std::vector<int> restarts;
  uint64_t counter;
  bool all_done;
};

template <typename TransportT, typename ProtocolT>
ReactorOutputs reactor_test1(
  bool connect_async, std::string_view input, std::string_view expected_output,
  std::deque<net::test::FdAction> actions,
  typename TransportT::config_t transport_cfg = typename TransportT::config_t{},
  typename ProtocolT::config_t protocol_cfg = typename ProtocolT::config_t{}
) noexcept
{
  std::deque<net::test::FdScript> scripts = {net::test::FdScript{
    .actions = actions,
    .sentinel = net::test::FdScript::sentinel_t::HUP,
    .input = {reinterpret_cast<const std::byte *>(input.data()), input.size()},
    .connect_async = connect_async,
  }};

  TestReactor<Connection<net::TestNet, TransportT, ProtocolT>> reactor(
    scripts, std::make_tuple(std::make_tuple(transport_cfg, protocol_cfg))
  );

  CHECK(!reactor.stopping);

  // determine the written output:
  std::string_view out_sv;

  if constexpr (std::is_same_v<TransportT, transport::ScriptedTransport>)
  {
    out_sv = std::string_view{
      transport_cfg->output->data(), transport_cfg->output->size()
    };
  }
  else
  {
    CHECK(0 < reactor.outputs().size());
    auto out = reactor.outputs()[0];

    out_sv =
      std::string_view{reinterpret_cast<const char *>(out.data()), out.size()};
  }

  // assert output
  CHECK_MESSAGE(
    out_sv == expected_output, "output:   ", utils::readable_ascii(out_sv),
    "\n          expected: ", utils::readable_ascii(expected_output)
  );

  // return a couple of extra outputs to validate:
  return {reactor.restarts, reactor.counter, reactor.all_done()};
}

} // namespace manet::reactor::test
