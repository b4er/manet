#include <atomic>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <tuple>
#include <xmmintrin.h>

#include <rigtorp/SPSCQueue.h>

#include <manet/logging.hpp>
#include <manet/protocol/websocket.hpp>
#include <manet/reactor.hpp>
#include <manet/reactor/connection.hpp>
#include <manet/transport/tls.hpp>

#include "binance_codec.hpp"
#include "config.hpp"

using Tls = manet::transport::tls::Tls;

using BinanceWebSocket =
  manet::protocol::websocket::WebSocket<binance::BinanceDepth>;

using BinanceSbeConnection =
  manet::reactor::Connection<Net, Tls, BinanceWebSocket>;

manet::reactor::Reactor<Net, BinanceSbeConnection> reactor;

/** queue for depth diffs from the SBE WebSocket connection
 * - producer: BinanceDepth running on the `reactor` on network thread
 * - consumer: `run_worker` on worker thread
 */
alignas(128) rigtorp::SPSCQueue<binance::DepthEvent> depth_queue{1u << 10};

alignas(128) std::atomic<bool> g_shutdown{false};

/* threads */

pthread_t g_main_thread;

bool pin_thread(int cpu_id)
{
  auto thread_id = pthread_self();
  manet::log::trace("pinning thread to cpu-{}", cpu_id);

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_id, &cpuset);

  int rc = pthread_setaffinity_np(thread_id, sizeof(cpuset), &cpuset);
  if (rc != 0)
  {
    manet::log::error("failed to pin thread to cpu-{}", cpu_id);
    return false;
  }

  return true;
}

/** network thread: run all registered connections on reactor */
void *run_net(void *arg)
{
  if (!arg)
  {
    std::fprintf(stderr, "run_net(nullptr): argument is NULL\n");
    std::fflush(stderr);
    std::abort();
  }

  auto *config = reinterpret_cast<Config *>(arg);

  // pin core
  auto cpu_id = config->net_cpu_id;
  if (cpu_id.has_value() && !pin_thread(cpu_id.value()))
  {
    std::fprintf(stderr, "failed to pin net to cpu-%d\n", cpu_id.value());
    std::fflush(stderr);
    std::abort();
  }

  // reactor loop: handle all connections
  reactor.run(
    config->net_config,
    std::make_tuple(
      manet::reactor::ConnectionConfig<Tls, BinanceWebSocket>{
        "stream-sbe.binance.com",
        9443,                     // Net
        "stream-sbe.binance.com", // Transport
        // Protocol
        {.path = "/ws/btcusdt@depth",
         .extra = {{"X-MBX-APIKEY", config->api_key}},
         // Codec
         .codec_config = &depth_queue}
      }
    )
  );

  pthread_kill(g_main_thread, SIGTERM);

  return nullptr;
}

/** worker thread: consumes depth events from depth_queue and logs them */
void *run_worker(void *arg)
{
  auto *worker_cpu_id = reinterpret_cast<std::optional<int> *>(arg);

  if (worker_cpu_id->has_value() && !pin_thread(worker_cpu_id->value()))
  {
    std::fprintf(
      stderr, "failed to pin worker to cpu-%d\n", worker_cpu_id->value()
    );
    std::fflush(stderr);
    std::abort();
  }

  while (!g_shutdown.load(std::memory_order_acquire))
  {
    if (auto *ev = depth_queue.front())
    {
      auto e = *ev;
      depth_queue.pop();
      manet::log::info(
        "{}: {} {} ({}) at {} ({})", e.event_time_ns,
        e.side == Side::ask ? "ask" : "bid", e.qty, e.qty_exp, e.price,
        e.price_exp
      );
    }
    else
    {
      _mm_pause();
    }
  }

  return nullptr;
}

/** spawn two threads (network & worker) and handle interrupts */
int main(int argc, char *argv[])
{
  pthread_t t_net, t_worker;
  g_main_thread = pthread_self();

  auto config = get_config(argc, argv);

  // signal mask for kill
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);

  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  // start threads:
  if (pthread_create(&t_worker, nullptr, run_worker, &config.worker_cpu_id))
  {
    fprintf(stderr, "cannot start worker thread\n");
    exit(1);
  }

  if (pthread_create(&t_net, nullptr, run_net, &config))
  {
    fprintf(stderr, "cannot start network thread\n");
    g_shutdown.store(true, std::memory_order_release);
    pthread_join(t_worker, nullptr);
    exit(1);
  }

  // wait until we get killed, then signal()
  int sig;
  sigwait(&set, &sig);

  manet::log::trace("signal halt (signal={})", sig);
  Net::signal();

  // wait for graceful shutdown, then kill worker:
  pthread_join(t_net, nullptr);
  manet::log::warn("net halted (signal={})", sig);

  g_shutdown.store(true, std::memory_order_release);
  pthread_join(t_worker, nullptr);

  return sig;
}
