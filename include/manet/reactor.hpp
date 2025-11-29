#pragma once

#include "logging.hpp"
#include "reactor/connection.hpp"

namespace manet::reactor
{

template <typename Transport, typename Protocol> struct ConnectionConfig
{
  std::string host;
  uint16_t port;

  typename Transport::config_t transport_config;
  typename Protocol::config_t protocol_config;
};

/** Statically known set of connections.
 *
 * `.run()` starts an infinite event loop polling the network for new edge
 * events and handles them. Gracefully closed connections are restarted.
 *
 * Heartbeat every ~6.3 seconds.
 *
 * `Net::stop()` will terminate the event loop.
 *
 * @tparam Net the network implementation (for example POSIX or F-Stack).
 *         Must satisfy Net.
 */
template <typename Net, typename... Connections> class Reactor
{
public:
  using net_config_t = typename Net::config_t;

  template <typename... Configs>
  void run(net_config_t &config, const std::tuple<Configs...> &configs)
  {
    manet::log::info("initialising net ({})", Net::name);
    Net::init(config);

    // initialise all connections
    try
    {
      init(configs, std::make_index_sequence<NUM_CONNECTIONS>{});
    }
    catch (...)
    {
      Net::stop();
      throw;
    }

    manet::log::info("entering poll loop");
    Net::run(loop, this);
  }

private:
  using event_t = typename Net::event_t;

  static constexpr std::size_t NUM_CONNECTIONS = sizeof...(Connections);
  static constexpr std::size_t NUM_EVENTS = NUM_CONNECTIONS + 1;

  std::tuple<std::optional<Connections>...> connections{};
  bool stopping = false;

  template <typename Configs, std::size_t... I>
  void init(const Configs &configs, std::index_sequence<I...>)
  {
    (init_connection<I>(std::get<I>(configs)), ...);
  }

  template <std::size_t I, typename Config>
  void init_connection(const Config &config)
  {
    using Conn = std::tuple_element_t<I, std::tuple<Connections...>>;

    auto &opt = std::get<I>(connections);
    opt.emplace(
      std::move(config.host), config.port, std::move(config.transport_config),
      std::move(config.protocol_config)
    );

    Conn *conn = std::addressof(*opt);
    conn->attach(static_cast<BaseConnection<Net> *>(conn));
  }

  std::array<event_t, NUM_EVENTS> events{};

  static inline uint64_t counter = 0;

  static int loop(void *data) noexcept
  {
    auto *self = static_cast<Reactor *>(data);

    int nevents = Net::poll(self->events.data(), NUM_EVENTS);
    if (nevents < 0)
    {
      manet::log::error("poll failed");
      Net::stop();
    }

    for (int i = 0; i < nevents; i++)
    {
      auto &ev = self->events[i];

      // when Posix this may drain the signalfd (posix)
      auto kill = Net::ev_signal(ev);

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
        auto conn = static_cast<BaseConnection<Net> *>(Net::get_user_data(ev));

        if (!conn->done())
        {
          conn->handle_event(ev);

          if (!self->stopping && conn->closed())
          {
            conn->restart();
          }
        }
      }

      if (self->stopping && self->all_done())
      {
        Net::stop();
      }
    }

    // ~every 6.3 seconds (aligned with timeouts)
    if ((++counter & 63) == 0)
    {
      self->heartbeat();
    }

    return 0;
  }

  bool all_done() const noexcept
  {
    return std::apply(
      [](auto const &...opts) { return ((!opts || opts->done()) && ...); },
      connections
    );
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
    manet::log::info("stopping all connections");
    std::apply(
      [](auto &...opt) { ((opt ? opt->stop() : void()), ...); }, connections
    );
  }
};

} // namespace manet::reactor
