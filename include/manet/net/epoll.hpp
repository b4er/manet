#pragma once

#include <cerrno>
#include <cstdio>
#include <sys/epoll.h>
#include <unistd.h>
#include <variant>

namespace manet::net
{

struct Epoll
{
  using config_t = std::monostate;
  using fd_t = int;
  using event_t = epoll_event;

  static constexpr const char *name = "Epoll";

  // sockets
  static fd_t socket(int domain, int type, int proto) noexcept;
  static int ioctl(fd_t fd, long req, void *argp) noexcept;
  static int connect(fd_t fd, const void *sa, socklen_t l) noexcept;
  static int close(fd_t fd) noexcept;

  static int getsockopt(
    fd_t fd, int level, int opt_name, void *opt_val, socklen_t *opt_len
  ) noexcept;

  static int read(fd_t fd, void *ptr, std::size_t len) noexcept;
  static int write(fd_t fd, const void *ptr, std::size_t len) noexcept;

  // reactor lifecycle
  static void init(config_t);
  static void run(int (*loop)(void *arg), void *arg);

  static void signal() noexcept;
  static void stop() noexcept;

  static int poll(event_t events[], std::size_t len) noexcept;

  // events
  static bool ev_signal(const event_t &ev) noexcept;
  static bool ev_close(const event_t &ev) noexcept;
  static bool ev_error(const event_t &ev) noexcept;
  static bool ev_readable(const event_t &ev) noexcept;
  static bool ev_writeable(const event_t &ev) noexcept;

  static void *get_user_data(const event_t &ev) noexcept;

  // event subscriptions
  static void
  subscribe(void *ptr, fd_t fd, bool want_read, bool want_write) noexcept
  {
    epoll_event ee{};

    ee.events = EPOLLERR | EPOLLHUP | EPOLLET;
    ee.data.ptr = ptr;

    if (want_read)
      ee.events |= EPOLLIN;
    if (want_write)
      ee.events |= EPOLLOUT;

    if (::epoll_ctl(_event_fd, EPOLL_CTL_MOD, fd, &ee) != 0)
    {
      switch (errno)
      {
      case ENOENT:
      {
        // not registered yet: ADD
        (void)::epoll_ctl(_event_fd, EPOLL_CTL_ADD, fd, &ee);
        break;
      }
      case EEXIST:
      {
        // retry: MOD
        (void)::epoll_ctl(_event_fd, EPOLL_CTL_MOD, fd, &ee);
        break;
      }
      case EBADF:
      {
        // non-sense, or raced (already closed/removed)
        break;
      }
      default:
      {
        char err_str[64];
        std::snprintf(
          err_str, 64, "epoll_ctl(ep=%d, op=MOD/ADD, fd=%d, _)", _event_fd, fd
        );
        perror(err_str);
        break;
      }
      }
    }
  }

  static void clear(fd_t fd) noexcept;

private:
  static int _event_fd;
  static int _signal_fd;
  static bool _alive;
};

} // namespace manet::net
