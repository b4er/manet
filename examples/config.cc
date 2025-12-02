#include <getopt.h>
#include <iostream>
#include <manet/logging.hpp>
#include <openssl/pem.h>

#include "config.hpp"

#ifdef MANET_USE_FSTACK
const char *short_opts = manet::log::enabled ? "+hc:v" : "+hc:";
#else
const char *short_opts = manet::log::enabled ? "+hv" : "+h";
#endif

struct option long_opts[] = {
  {"help", no_argument, nullptr, 'h'},
#ifdef MANET_USE_FSTACK
  {"config", required_argument, nullptr, 'c'}, // forward to F-Stack
#endif
  {"net-cpu", required_argument, nullptr, 'n'},
  {"worker-cpu", required_argument, nullptr, 'w'},
  {0, 0, 0, 0}
};

void helpful_exit(char *pname, int status)
{
  auto fout = status == 0 ? stdout : stderr;

  fprintf(fout, "usage: %s [options]\n\n", pname);
  fprintf(fout, "net: %s\n\n", Net::name);
  fprintf(fout, "options:\n");
  fprintf(fout, "  -h, --help            show help\n");
#ifdef MANET_USE_FSTACK
  fprintf(fout, "  -c <conf>             F-Stack config file\n");
#endif
  fprintf(fout, "  --net-cpu <id>        pin network thread to CPU <id>\n");
  fprintf(fout, "  --worker-cpu <id>     pin worker thread to CPU <id>\n");
  if (manet::log::enabled)
  {
    fprintf(fout, "  -v|-vv              set verbose\n");
  }

  exit(status);
}

struct Args
{
  Net::config_t net_config;
  std::optional<int> net_cpu;
  std::optional<int> worker_cpu;
};

Args read_args(int argc, char *argv[])
{
  Args args;
#ifdef MANET_USE_FSTACK
  args.net_config = {};
#endif
  args.net_cpu = {};
  args.worker_cpu = {};

  int c;
  int v_count = 0;

  while ((c = getopt_long(argc, argv, short_opts, long_opts, nullptr)) != -1)
  {
    switch (c)
    {
    case 'w':
      args.worker_cpu = std::stoi(optarg);
      break;
    case 'n':
      args.net_cpu = std::stoi(optarg);
      break;
    case 'h':
      helpful_exit(argv[0], 0);
      break;
#ifdef MANET_USE_FSTACK
    case 'c':
      args.net_config = optarg;
      break;
#endif
    case 'v':
      v_count++;
      break;
    default:
      helpful_exit(argv[0], -1);
    }
  }

  if (v_count)
  {
    manet::log::set_level(
      v_count == 1 ? manet::log::LogLevel::info : manet::log::LogLevel::trace
    );
  }

  return args;
}

EVP_PKEY *load_ed25519_key(const char *path) noexcept
{
  FILE *file = fopen(path, "r");
  if (!file)
  {
    manet::log::error("invalid PEM file: {}", path);
    return nullptr;
  }

  EVP_PKEY *private_key = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
  fclose(file);

  return private_key;
}

Config get_config(int argc, char *argv[])
{
  auto args = read_args(argc, argv);

  // read Binance API key from environment
  const char *api_cstr = std::getenv("MBX_APIKEY");

  auto api_key = api_cstr ? std::string{api_cstr} : "";
  if (api_key == "")
  {
    std::cerr << "MBX_APIKEY is not set!" << '\n';
    exit(1);
  }

  /* load the corresponding private key from file
  auto private_key = PrivateKey(load_ed25519_key(".binance-ed25519"));
  if (!private_key)
  {
    std::cerr << "could not load private key\n";
    exit(1);
  }*/

  return Config{
    .net_config = args.net_config,
    .api_key = std::move(api_key),
    // std::move(private_key),
    .net_cpu_id = args.net_cpu,
    .worker_cpu_id = args.worker_cpu
  };
}
