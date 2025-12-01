#pragma once

#include <memory>
#include <openssl/evp.h>

#ifdef USE_FSTACK
#include "manet/net/fstack.hpp"
using Net = manet::net::FStack;
#else
#include "manet/net/epoll.hpp"
using Net = manet::net::Epoll;
#endif

struct EVP_PKEY_delete
{
  void operator()(EVP_PKEY *pkey) const noexcept
  {
    if (pkey)
      EVP_PKEY_free(pkey);
  }
};

using PrivateKey = std::unique_ptr<EVP_PKEY, EVP_PKEY_delete>;

struct Config
{
  Net::config_t net_config;
  std::string api_key;
  // PrivateKey private_key;

  std::optional<int> net_cpu_id;
  std::optional<int> worker_cpu_id;
};

Config get_config(int argc, char *argv[]);
