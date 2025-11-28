#include <atomic>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <tuple>

#include <manet/reactor.hpp>
#include <manet/utils/logging.hpp>

#include "config.hpp"

manet::reactor::Reactor<Net> reactor;

void *net_loop(void *arg)
{
  auto *config = reinterpret_cast<Config *>(arg);

  reactor.run(config->net_config, std::make_tuple());

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

  manet::utils::trace("net signal halt", sig);
  Net::signal();

  // wait for graceful shutdown:
  pthread_join(t_net, nullptr);
  manet::utils::warn("net halted (signal={})", sig);

  return sig;
}
