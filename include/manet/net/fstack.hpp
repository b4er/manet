#pragma once

#include <optional>
#include <string>

#include "ff_api.h"

#include "manet/utils/logging.hpp"

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

  static std::size_t read(fd_t fd, void *buf, std::size_t len) noexcept;
  static std::size_t write(fd_t fd, const void *buf, std::size_t len) noexcept;

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
    struct kevent kev[2];
    int k = 0;

    // delete subscription(s):

    if (!want_read)
    {
      EV_SET(&kev[k++], fd, EVFILT_READ, EV_DELETE, 0, 0, ptr);
    }

    if (!want_write)
    {
      EV_SET(&kev[k++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, ptr);
    }

    if (0 < k)
    {
      (void)ff_kevent(_kq, kev, k, nullptr, 0, nullptr);
    }

    k = 0;

    // add subscription(s):

    if (want_read)
    {
      EV_SET(
        &kev[k++], fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, ptr
      );
    }

    if (want_write)
    {
      EV_SET(
        &kev[k++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, ptr
      );
    }

    if (k == 0)
      return;

    if (ff_kevent(_kq, kev, k, nullptr, 0, nullptr) < 0)
    {
      switch (errno)
      {
      case EEXIST:
      {
        // already exists:
        for (int i = 0; i < k; ++i)
        {
          EV_SET(&kev[i], fd, kev[i].filter, EV_ENABLE, 0, 0, ptr);
        }

        (void)ff_kevent(_kq, kev, k, nullptr, 0, nullptr);
        return;
      }
      case EBADF:
        break;
      default:
        manet::utils::error(
          "FStack::subscribe(_, {}, {}, {}) failed", fd, want_read, want_write
        );
        break;
      }
    }
  }

  static void clear(fd_t fd) noexcept;

private:
  static int _kq;
};

} // namespace manet::net
