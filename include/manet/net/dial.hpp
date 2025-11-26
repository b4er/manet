#pragma once

#include <netdb.h>
#include <string_view>
#include <sys/ioctl.h>

namespace manet::net
{

template <typename Net> struct DialResult
{
  typename Net::fd_t fd;
  int err;
};

template <typename Net>
DialResult<Net> dial(std::string_view host, uint16_t port) noexcept
{
  DialResult<Net> result{};
  result.fd = -1;
  result.err = ECONNREFUSED;

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_ADDRCONFIG;

  char sport[16] = {};
  snprintf(sport, sizeof sport, "%u", static_cast<unsigned>(port));

  struct addrinfo *res = NULL, *ai = NULL;
  int gai = getaddrinfo(host.data(), sport, &hints, &res);

  if (gai != 0)
  {
    result.err = EINVAL;
    return result;
  }

  for (ai = res; ai != nullptr; ai = ai->ai_next)
  {
    // Net::fd_t ~ int
    int socketfd = Net::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

    if (socketfd < 0)
    {
      result.err = errno;
      continue;
    }

    // set non-blocking
    int on = 1;
    if (Net::ioctl(socketfd, FIONBIO, &on) < 0)
    {
      result.err = errno;
      Net::close(socketfd);
      continue;
    }

    // attempt connection
    int ok = Net::connect(socketfd, ai->ai_addr, ai->ai_addrlen);
    if (ok == 0)
    {
      result.err = 0;
      result.fd = socketfd;
      break;
    }

    result.err = errno;

    if (errno == EINPROGRESS)
    {
      result.fd = socketfd;
      break;
    }

    Net::close(socketfd);
  }

  freeaddrinfo(res);

  return result;
}

} // namespace manet::net
