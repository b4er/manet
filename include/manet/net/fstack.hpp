#pragma once

#include <optional>
#include <string>

#include "ff_api.h"

namespace manet::net
{

struct FStack
{
  using config_t = std::optional<std::string>;
  using event_t = kevent;
  using fd_t = int;

  static constexpr const char *name = "F-Stack";

  // sockets
  static fd_t socket(int domain, int type, int proto) noexcept;
  static int ioctl(fd_t fd, long req, void *argp) noexcept;
  static int connect(fd_t fd, const void *sa, socklen_t l) noexcept;
  static int close(fd_t fd) noexcept;

  static int getsockopt(
    fd_t fd, int level, int opt_name, void *opt_val, socklen_t *opt_len
  ) noexcept;

  static int read(fd_t fd, void *buf, std::size_t len) noexcept;
  static int write(fd_t fd, const void *buf, std::size_t len) noexcept;

  // reactor lifecycle
  static void init(config_t config);
  static void run(int (*loop)(void *arg), void *arg);

  static int poll(event_t events[], std::size_t len) noexcept;

  static void signal() noexcept;
  static void stop() noexcept;

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
    static_assert(want_read != want_write);

    struct kevent kev[2];

    if (want_read)
    {
      EV_SET(&kev[0], fd, EVFILT_WRITE, EV_DELETE, 0, 0, ptr);
      EV_SET(
        &kev[0], fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, ptr
      );
    }
    else
    {
      EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, ptr);
      EV_SET(
        &kev[0], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, ptr
      );
    }

    if (ff_kevent(_kq, kev, 2, nullptr, 0, nullptr) < 0)
    {
      switch (errno)
      {
      case ENOENT:
      case EBADF:
        // non-sense, or raced (already closed/removed)
        return;
      case EEXIST:
        Log::log(LogLevel::warn, "ff_kevent(flags={}) -> EEXIST", flags);
        if (flags & EV_ADD)
        {
          EV_SET(&kev, fd, events, EV_ENABLE, 0, 0, ptr);
          if (ff_kevent(kq, &kev, 1, NULL, 0, NULL) == 0)
            return;
        }
        break;
      default:
      {
        char err_str[64];
        std::snprintf(
          err_str, 64, "ff_kevent(kq=%d, flags=%d, fd=%d, _)", kq, flags, fd
        );
        perror(err_str);
      }
      }
    }
  }

  static void clear(fd_t fd) noexcept;

private:
  static int _kq;
};

} // namespace manet::net
