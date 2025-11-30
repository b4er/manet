#include <atomic>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <tuple>

#include <manet/logging.hpp>
#include <manet/protocol/websocket.hpp>
#include <manet/reactor.hpp>
#include <manet/reactor/connection.hpp>
#include <manet/transport/tls.hpp>

#include "binance_codec.hpp"
#include "config.hpp"

using Tls = manet::transport::tls::Tls;

using BinanceWebSocket = manet::protocol::websocket::WebSocket<
  manet::protocol::websocket::BinanceDepth>;

using BinanceSbeConnection =
  manet::reactor::Connection<Net, Tls, BinanceWebSocket>;

manet::reactor::Reactor<Net, BinanceSbeConnection> reactor;

void *net_loop(void *arg)
{
  auto *config = reinterpret_cast<Config *>(arg);

  reactor.run(
    config->net_config,
    std::make_tuple(
      manet::reactor::ConnectionConfig<Tls, BinanceWebSocket>{
        "stream-sbe.binance.com",
        9443,                     // Net
        "stream-sbe.binance.com", // Transport
        // Protocol
        {.path = "/ws/btcusdt@depth",
         .extra = {{"X-MBX-APIKEY", config->api_key}}}
      }
    )
  );

  pthread_exit(nullptr);

  return nullptr;
}

int main(int argc, char *argv[])
{
  pthread_t t_net;

  auto config = get_config(argc, argv);

  // signal mask for kills
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);

  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  // start threads
  if (pthread_create(&t_net, nullptr, net_loop, &config))
  {
    fprintf(stderr, "cannot start network thread\n");
    exit(1);
  }

  // wait until we get killed, then signal()
  int sig;
  sigwait(&set, &sig);

  manet::log::trace("net signal halt", sig);
  Net::signal();

  // wait for graceful shutdown:
  pthread_join(t_net, nullptr);
  manet::log::warn("net halted (signal={})", sig);

  return sig;
}
