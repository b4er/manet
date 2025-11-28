#include <cstring>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "manet/logging.hpp"
#include "manet/net/concepts.hpp"
#include "manet/net/epoll.hpp"

namespace manet::net
{

int Epoll::_event_fd = -1;
int Epoll::_signal_fd = -1;
bool Epoll::_alive = false;

/* sockets */

Epoll::fd_t Epoll::socket(int domain, int type, int proto) noexcept
{
  return ::socket(domain, type | SOCK_NONBLOCK, proto);
}

int Epoll::ioctl(fd_t fd, long req, void *argp) noexcept
{
  return ::ioctl(fd, req, argp);
}

int Epoll::connect(fd_t fd, const void *sa, socklen_t l) noexcept
{
  return ::connect(fd, (const sockaddr *)sa, l);
}

int Epoll::close(fd_t fd) noexcept { return ::close(fd); }

int Epoll::getsockopt(
  fd_t fd, int level, int opt_name, void *opt_val, socklen_t *opt_len
) noexcept
{
  return ::getsockopt(fd, level, opt_name, opt_val, opt_len);
}

std::size_t Epoll::read(fd_t fd, void *buf, std::size_t len) noexcept
{
  return ::read(fd, buf, len);
}

std::size_t Epoll::write(fd_t fd, const void *buf, std::size_t len) noexcept
{
  return ::write(fd, buf, len);
}

/* lifecycle */

void Epoll::init(std::monostate)
{
  _event_fd = epoll_create1(EPOLL_CLOEXEC);
  if (_event_fd < 0)
  {
    throw std::runtime_error("failed to create epoll fd");
  }

  _signal_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (_signal_fd < 0)
  {
    throw std::runtime_error("failed to create kill eventfd");
  }

  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = _signal_fd;

  if (epoll_ctl(_event_fd, EPOLL_CTL_ADD, _signal_fd, &ev) < 0)
  {
    throw std::runtime_error("failed to subscribe to killfd");
  }

  _alive = true;
}

void Epoll::run(int (*loop)(void *arg), void *arg)
{
  while (_alive)
  {
    loop(arg);
  }
}

void Epoll::signal() noexcept
{
  if (_signal_fd != -1)
  {
    uint64_t one = 1;
    write(_signal_fd, &one, sizeof(one));
  }
}

void Epoll::stop() noexcept
{
  _alive = false;

  if (_signal_fd != -1)
  {
    ::close(_signal_fd);
    _signal_fd = -1;
  }

  if (_event_fd != -1)
  {
    ::close(_event_fd);
    _event_fd = -1;
  }
}

int Epoll::poll(event_t events[], std::size_t len) noexcept
{
  constexpr auto max_int_size_t =
    static_cast<std::size_t>(std::numeric_limits<int>::max());

  // clamp
  len = len < max_int_size_t ? len : max_int_size_t;

  return epoll_wait(
    _event_fd, events, static_cast<int>(len), poll_frequency_ms
  );
}

/* events */

bool Epoll::ev_signal(const event_t &ev) noexcept
{
  if (ev.data.fd == _signal_fd && (ev.events & EPOLLIN))
  {
    // drain fd
    uint64_t discard;
    std::size_t n = ::read(_signal_fd, &discard, sizeof(discard));
    (void)n;

    return true;
  }

  return false;
}

bool Epoll::ev_close(const event_t &ev) noexcept
{
  return (ev.events & (EPOLLHUP | EPOLLRDHUP)) != 0;
}

bool Epoll::ev_error(const event_t &ev) noexcept
{
  return (ev.events & EPOLLERR) != 0;
}

bool Epoll::ev_readable(const event_t &ev) noexcept
{
  return (ev.events & EPOLLIN) != 0;
}

bool Epoll::ev_writeable(const event_t &ev) noexcept
{
  return (ev.events & EPOLLOUT) != 0;
}

void *Epoll::get_user_data(const event_t &ev) noexcept { return ev.data.ptr; }

void Epoll::clear(fd_t fd) noexcept
{
  ::epoll_ctl(_event_fd, EPOLL_CTL_DEL, fd, nullptr);
}

} // namespace manet::net
