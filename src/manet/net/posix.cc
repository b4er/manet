#include <cstring>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "manet/net/posix.hpp"
#include "manet/utils/logging.hpp"

namespace manet::net
{

int Posix::_event_fd = -1;
int Posix::_signal_fd = -1;
bool Posix::_alive = false;

/* sockets */

Posix::fd_t Posix::socket(int domain, int type, int proto) noexcept
{
  return ::socket(domain, type | SOCK_NONBLOCK, proto);
}

int Posix::ioctl(fd_t fd, long req, void *argp) noexcept
{
  return ::ioctl(fd, req, argp);
}

int Posix::connect(fd_t fd, const void *sa, socklen_t l) noexcept
{
  return ::connect(fd, (const sockaddr *)sa, l);
}

int Posix::close(fd_t fd) noexcept { return ::close(fd); }

int Posix::getsockopt(
  fd_t fd, int level, int opt_name, void *opt_val, socklen_t *opt_len
) noexcept
{
  return ::getsockopt(fd, level, opt_name, opt_val, opt_len);
}

int Posix::read(fd_t fd, void *buf, std::size_t len) noexcept
{
  return ::read(fd, buf, len);
}

int Posix::write(fd_t fd, const void *buf, std::size_t len) noexcept
{
  return ::write(fd, buf, len);
}

/* lifecycle */

void Posix::init(std::monostate)
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

void Posix::run(int (*loop)(void *arg), void *arg)
{
  while (_alive)
  {
    loop(arg);
  }
}

void Posix::signal() noexcept
{
  if (_signal_fd != -1)
  {
    uint64_t one = 1;
    write(_signal_fd, &one, sizeof(one));
  }
}

void Posix::stop() noexcept
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

int Posix::poll(event_t events[], std::size_t len) noexcept
{
  return epoll_wait(_event_fd, events, len, 100);
}

/* events */

bool Posix::ev_signal(const event_t &ev) noexcept
{
  if (ev.data.fd == _signal_fd && (ev.events & EPOLLIN))
  {
    // drain fd
    uint64_t discard;
    (void)::read(_signal_fd, &discard, sizeof(discard));

    return true;
  }

  return false;
}

bool Posix::ev_close(const event_t &ev) noexcept
{
  return (ev.events & (EPOLLHUP | EPOLLRDHUP)) != 0;
}

bool Posix::ev_error(const event_t &ev) noexcept
{
  return (ev.events & EPOLLERR) != 0;
}

bool Posix::ev_readable(const event_t &ev) noexcept
{
  return (ev.events & EPOLLIN) != 0;
}

bool Posix::ev_writeable(const event_t &ev) noexcept
{
  return (ev.events & EPOLLOUT) != 0;
}

void *Posix::get_user_data(const event_t &ev) noexcept { return ev.data.ptr; }

void Posix::clear(fd_t fd) noexcept
{
  ::epoll_ctl(_event_fd, EPOLL_CTL_DEL, fd, nullptr);
}

} // namespace manet::net
