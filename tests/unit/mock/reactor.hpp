#pragma once
#include <doctest/doctest.h>

#include <utility>

#include "net.hpp"
#include "transport.hpp"

template <typename... Connections> class TestReactor
{
public:
  using event_t = typename TestNet::event_t;

  static constexpr std::size_t NUM_CONNECTIONS = sizeof...(Connections);
  static constexpr std::size_t NUM_EVENTS = NUM_CONNECTIONS + 1;

  std::tuple<std::optional<Connections>...> connections{};
  std::array<event_t, NUM_EVENTS> events{};

  static inline uint64_t counter = 0;
  bool stopping = false;

  // test outputs
  std::vector<int> restarts = {};

  template <typename... Configs>
  TestReactor(TestNet::config_t &config, const std::tuple<Configs...> &cfgs)
  {
    TestNet::init(config);
    init(cfgs, std::make_index_sequence<NUM_CONNECTIONS>{});
    TestNet::run(loop, this);
  }

  static std::vector<std::span<const std::byte>> outputs() noexcept
  {
    std::vector<std::span<const std::byte>> res;
    res.reserve(NUM_CONNECTIONS);

    for (std::size_t i = 0; i < NUM_CONNECTIONS; i++)
    {
      res.emplace_back(TestNet::_output(i));
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

    std::string host = "localhost";
    uint16_t port = 101;

    auto &opt = std::get<I>(connections);
    opt.emplace(
      std::move(host), port, std::move(std::get<0>(cfg)),
      std::move(std::get<1>(cfg))
    );

    Conn *conn = std::addressof(*opt);
    conn->attach(static_cast<manet::reactor::BaseConnection<TestNet> *>(conn));

    _conn_ids[conn] = I;
  }

  static int loop(void *data) noexcept
  {
    auto *self = static_cast<TestReactor *>(data);

    int nevents = TestNet::poll(self->events.data(), NUM_EVENTS);
    if (nevents < 0)
    {
      manet::utils::error("poll failed");
      TestNet::stop();
    }

    for (int i = 0; i < nevents; i++)
    {
      auto &ev = self->events[i];

      auto kill = TestNet::ev_signal(ev);
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
        void *ptr = TestNet::get_user_data(ev);
        auto conn = static_cast<manet::reactor::BaseConnection<TestNet> *>(ptr);

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
        TestNet::stop();
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

template <typename Transport, typename Protocol>
ReactorOutputs test1(
  bool connect_async, std::string_view input, std::string_view expected_output,
  std::deque<FdAction> actions,
  typename Transport::config_t transport_cfg = typename Transport::config_t{},
  typename Protocol::config_t protocol_cfg = typename Protocol::config_t{}
) noexcept
{
  std::deque<FdScript> scripts = {FdScript{
    .actions = std::move(actions),
    .sentinel = FdScript::sentinel_t::HUP,
    .input = {reinterpret_cast<const std::byte *>(input.data()), input.size()},
    .connect_async = connect_async,
  }};

  TestReactor<manet::reactor::Connection<TestNet, Transport, Protocol>> reactor(
    scripts, std::make_tuple(std::make_tuple(transport_cfg, protocol_cfg))
  );

  CHECK(!reactor.stopping);

  // determine the written output:
  std::string_view out_sv;

  if constexpr (std::is_same_v<Transport, ScriptedTransport>)
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
    out_sv == expected_output,
    "output:   ", manet::utils::readable_ascii(out_sv),
    "\n          expected: ", manet::utils::readable_ascii(expected_output)
  );

  // return a couple of extra outputs to validate:
  return {reactor.restarts, reactor.counter, reactor.all_done()};
}
