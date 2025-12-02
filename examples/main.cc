#include <atomic>
#include <cinttypes>
#include <csignal>
#include <cstring>
#include <iostream>
#include <optional>
#include <pthread.h>
#include <sched.h>
#include <tuple>
#include <xmmintrin.h>

#include <rigtorp/SPSCQueue.h>

#include <manet/protocol/websocket.hpp>
#include <manet/reactor.hpp>
#include <manet/transport/tls.hpp>

#include "binance_codec.hpp"
#include "config.hpp"

// websocket connection using the codec defined in binance_codec.hpp
using BinanceWebSocket = manet::protocol::WebSocket<binance::BinanceDepth>;

// reactor that holds a single wss::/<BinanceDepth> connection
using Reactor = manet::Reactor<
  Net, manet::Connection<Net, manet::transport::Tls, BinanceWebSocket>>;

struct AppContext
{
  Reactor reactor;
  Config config;

  /** queue for depth diffs from the SBE WebSocket connection
   * - producer: BinanceDepth running on the `reactor` on network thread
   * - consumer: `run_worker` on worker thread
   */
  rigtorp::SPSCQueue<binance::DepthEvent> &depth_queue;

  /** worker shutdown flag */
  std::atomic<bool> &shutdown;
};

/* threads */

pthread_t g_main_thread;

/** ideally we pin our threads to isolcpus */
bool pin_thread(std::optional<int> opt_cpu_id)
{
  if (!opt_cpu_id.has_value())
    return true;

  auto cpu_id = opt_cpu_id.value();

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_id, &cpuset);

  int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  if (rc != 0)
  {
    std::fprintf(
      stderr, "failed to pin thread to cpu-%d: %s\n", cpu_id, std::strerror(rc)
    );
    std::fflush(stderr);
    return false;
  }

  return true;
}

/** network thread: run all registered connections on reactor */
void *run_net(void *arg)
{
  auto *context = static_cast<AppContext *>(arg);

  auto &config = context->config;
  auto &depth_queue = context->depth_queue;
  auto &reactor = context->reactor;

  if (!pin_thread(config.net_cpu_id))
  {
    std::abort();
  }

  // reactor loop: handle all connections
  reactor.run(
    config.net_config,
    std::make_tuple(
      manet::ConnectionConfig<manet::transport::Tls, BinanceWebSocket>{
        "stream-sbe.binance.com",
        9443,                     // Net
        "stream-sbe.binance.com", // Transport
        // Protocol
        {.path = "/ws/btcusdt@depth",
         .extra = {{"X-MBX-APIKEY", config.api_key}},
         // Codec
         .codec_config = &depth_queue}
      }
    )
  );

  pthread_kill(g_main_thread, SIGUSR1);

  return nullptr;
}

/** worker thread: consumes depth events from depth_queue and logs them */
void *run_worker(void *arg)
{
  auto *context = static_cast<AppContext *>(arg);

  auto &config = context->config;
  auto &depth_queue = context->depth_queue;
  auto &shutdown = context->shutdown;

  if (!pin_thread(config.worker_cpu_id))
  {
    std::abort();
  }

  while (!shutdown.load(std::memory_order_acquire))
  {
    if (auto *ev = depth_queue.front())
    {
      auto e = *ev;
      depth_queue.pop();

      // clang-format off
      std::printf(
        "%" PRId64 ": %s %" PRId64 "e%" PRId64 " @ %" PRId64 "e%" PRId64 "\n",
        e.event_time_ns,
        e.side == Side::ask ? "A" : "B",
        e.qty,
        e.qty_exp,
        e.price,
        e.price_exp
      );
      // clang-format on
    }
    else
    {
      // assume we do not care about burning this core (otw. use yield())
      _mm_pause();
    }
  }

  return nullptr;
}

/** spawn two threads (network & worker) and handle interrupts */
int main(int argc, char *argv[])
{
  auto config = get_config(argc, argv);

  alignas(128) rigtorp::SPSCQueue<binance::DepthEvent> queue{1u << 10};
  alignas(128) std::atomic<bool> shutdown{false};

  AppContext context{Reactor{}, config, queue, shutdown};

  pthread_t t_net, t_worker;
  g_main_thread = pthread_self();

  // signal mask for kill
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGUSR1);

  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  // start threads:
  if (pthread_create(&t_worker, nullptr, run_worker, &context))
  {
    std::fprintf(stderr, "cannot start worker thread\n");
    exit(1);
  }

  if (pthread_create(&t_net, nullptr, run_net, &context))
  {
    std::fprintf(stderr, "cannot start network thread\n");
    shutdown.store(true, std::memory_order_release);
    pthread_join(t_worker, nullptr);
    exit(1);
  }

  // wait until we get killed, then signal()
  int sig;
  sigwait(&set, &sig);
  if (sig != SIGUSR1)
  {
    Net::signal();
  }

  // wait for graceful shutdown, then kill worker:
  pthread_join(t_net, nullptr);
  std::fprintf(stderr, "net halted (signal=%d)\n", sig);

  shutdown.store(true, std::memory_order_release);
  pthread_join(t_worker, nullptr);

  return (sig == SIGINT || sig == SIGTERM) ? 0 : 1;
}
