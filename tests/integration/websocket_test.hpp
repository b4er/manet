#include <doctest/doctest.h>
#include <pthread.h>
#include <semaphore.h>
#include <sstream>
#include <string>
#include <vector>

#include <manet/protocol/websocket.hpp>
#include <manet/reactor.hpp>
#include <manet/reactor/connection.hpp>
#include <manet/transport/tls.hpp>

namespace manet::protocol::websocket::test
{

struct WsTest
{
  struct config_t
  {
    sem_t *done = nullptr;
    std::vector<std::string> *output = nullptr;
  };

  sem_t *done = nullptr;
  std::vector<std::string> *output = nullptr;

  WsTest(config_t config) noexcept
      : done(config.done),
        output(config.output)
  {
  }

protected:
  void write(std::string_view s) noexcept
  {
    if (output)
      output->emplace_back(s);
  }

  void signal_done() noexcept
  {
    if (done)
      sem_post(done);
  }
};

template <class T, class...> using repeat_t = T;

template <typename Transport, typename Codec> struct WsConn
{
};

template <typename WsConn> struct SplitWsConn;

template <typename Transport, typename Codec>
struct SplitWsConn<WsConn<Transport, Codec>>
{
  using transport_t = Transport;
  // using codec_t  = Codec;
  using protocol_t = protocol::websocket::WebSocket<Codec>;
};

template <typename Net, typename Pair> struct MakeWsConnection;

template <typename Net, typename Transport, typename Codec>
struct MakeWsConnection<Net, WsConn<Transport, Codec>>
{
  using type =
    reactor::Connection<Net, Transport, protocol::websocket::WebSocket<Codec>>;
};

template <typename Net, typename... WsConnections> class ConnectionsTest
{
public:
  ConnectionsTest(auto paths)
      : _paths(paths)
  {
    // init all semaphores first (before starting reactor)
    [&]<std::size_t... Is>(std::index_sequence<Is...>)
    {
      ((sem_init(&std::get<Is>(_dones), 0, 0) == 0
          ? void()
          : throw std::runtime_error("sem_init failed")),
       ...);
    }(std::index_sequence_for<WsConnections...>{});

    pthread_t t_reactor;
    int err = pthread_create(
      &t_reactor, nullptr, &ConnectionsTest::reactor_worker, this
    );
    CHECK_MESSAGE(err == 0, "must be able to start reactor thread");

    // await all semaphores
    [&]<std::size_t... Is>(std::index_sequence<Is...>)
    {
      (sem_wait(&std::get<Is>(_dones)), ...);
    }(std::index_sequence_for<WsConnections...>{});

    // signal that we're done and await shutdownd
    Net::signal();
    pthread_join(t_reactor, nullptr);
  }

  template <std::size_t I> auto const &output() const
  {
    return std::get<I>(_outputs);
  }

private:
  reactor::Reactor<Net, typename MakeWsConnection<Net, WsConnections>::type...>
    _reactor;

  std::tuple<repeat_t<std::string, WsConnections>...> _paths;
  std::tuple<repeat_t<sem_t, WsConnections>...> _dones;
  std::tuple<repeat_t<std::vector<std::string>, WsConnections>...> _outputs;

  std::string host = "localhost";

  static void *reactor_worker(void *data)
  {
    std::monostate net_config{};
    auto *self = static_cast<ConnectionsTest *>(data);
    self->_reactor.run(net_config, self->make_configs_tuple());

    // reactor stopped before posting -> release
    [&]<std::size_t... Is>(std::index_sequence<Is...>)
    {
      (sem_post(&std::get<Is>(self->_dones)), ...);
    }(std::index_sequence_for<WsConnections...>{});

    return nullptr;
  }

  auto make_configs_tuple()
  {
    return [&]<std::size_t... Is>(std::index_sequence<Is...>)
    {
      return std::make_tuple(make_config<Is, WsConnections>()...);
    }(std::index_sequence_for<WsConnections...>{});
  }

  template <std::size_t I, typename WsConnection> auto make_config()
  {
    using C = SplitWsConn<WsConnection>;

    using Transport = typename C::transport_t;
    using Protocol = typename C::protocol_t;

    using config_t = reactor::ConnectionConfig<Transport, Protocol>;

    auto &done = std::get<I>(_dones);
    auto &output = std::get<I>(_outputs);

    typename Protocol::config_t config{
      .path = std::get<I>(_paths),
      .extra = {},
      .codec_config = {.done = &done, .output = &output}
    };

    if constexpr (std::is_same_v<Transport, transport::tls::Tls>)
    {
      return config_t{
        .host = host,
        .port = 9443,
        .transport_config = host.c_str(),
        .protocol_config = config,
      };
    }
    else
    {
      return config_t{
        .host = host,
        .port = 9000,
        .transport_config = std::monostate{},
        .protocol_config = config,
      };
    }
  }
};

} // namespace manet::protocol::websocket::test

// a WebSocket<Codec> run leaves us with an output trace
using Trace = std::vector<std::string>;

// make the traces show pretty in CHECKs
namespace doctest
{
template <> struct StringMaker<Trace>
{
  static String convert(Trace const &v)
  {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (auto const &s : v)
    {
      if (!first)
        oss << ", ";
      first = false;
      oss << '"' << s << '"';
    }
    oss << "}";
    return oss.str().c_str();
  }
};

} // namespace doctest
