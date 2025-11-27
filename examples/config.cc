#include <getopt.h>
#include <iostream>
#include <manet/utils/logging.hpp>

#include "config.hpp"

#ifdef USE_FSTACK
const char *short_opts = manet::utils::logging_enabled ? "+hc:v" : "+hc:";
#else
const char *short_opts = manet::utils::logging_enabled ? "+hv" : "+h";
#endif

struct option long_opts[] = {
  {"help", no_argument, nullptr, 'h'},
#ifdef USE_FSTACK
  {"config", required_argument, nullptr, 'c'}, // forward to F-Stack
#endif
  {0, 0, 0, 0}
};

void helpful_exit(char *pname, int status)
{
  auto fout = status == 0 ? stdout : stderr;

  fprintf(fout, "usage: %s [options]\n\n", pname);
  fprintf(fout, "net: %s\n\n", Net::name);
  fprintf(fout, "options:\n");
  fprintf(fout, "  -h, --help            show help\n");
#ifdef USE_FSTACK
  fprintf(fout, "  -c <conf>             F-Stack config file\n");
#endif
  if (manet::utils::logging_enabled)
  {
    fprintf(fout, "  -v|-vv              set verbose\n");
  }

  exit(status);
}

Net::config_t read_args(int argc, char *argv[])
{
  int c;
#ifdef USE_FSTACK
  char *config_file = nullptr;
#endif

  int v_count = 0;

  while ((c = getopt_long(argc, argv, short_opts, long_opts, nullptr)) != -1)
  {
    switch (c)
    {
    case 'h':
      helpful_exit(argv[0], 0);
      break;
#ifdef USE_FSTACK
    case 'c':
      config_file = optarg;
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
    manet::utils::set_level(
      v_count == 1 ? manet::utils::LogLevel::info
                   : manet::utils::LogLevel::trace
    );
  }

#ifdef USE_FSTACK
  return config_file;
#else
  return {};
#endif
}

Config get_config(int argc, char *argv[])
{
  auto net_config = read_args(argc, argv);

  // read Binance API key from environment
  const char *api_cstr = std::getenv("MBX_APIKEY");

  auto api_key = api_cstr ? std::string{api_cstr} : "";
  if (api_key == "")
  {
    std::cerr << "MBX_APIKEY is not set!" << std::endl;
    exit(1);
  }

  /*
  // load the corresponding private key from file
  auto private_key = PrivateKey(load_ed25519_key(".binance-ed25519"));
  if (!private_key)
  {
    std::cerr << "could not load private key" << std::endl;
    exit(1);
  }
  */

  return Config{net_config, std::move(api_key)}; //, std::move(private_key)};
}
