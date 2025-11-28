#include <stdexcept>

#include "manet/net/concepts.hpp"
#include "manet/net/fstack.hpp"
#include "manet/utils/logging.hpp"

#define KILL_IDENT 1

namespace manet::net
{

int FStack::_kq = -1;

// sockets

FStack::fd_t FStack::socket(int domain, int type, int proto) noexcept
{
  return ff_socket(domain, type, proto);
}

int FStack::ioctl(fd_t fd, long req, void *arg) noexcept
{
  return ff_ioctl(fd, req, arg);
}

int FStack::connect(fd_t fd, const void *sa, socklen_t l) noexcept
{
  return ff_connect(fd, (const linux_sockaddr *)sa, l);
}

int FStack::close(fd_t fd) noexcept { return ff_close(fd); }

int FStack::getsockopt(
  fd_t fd, int level, int opt_name, void *opt_val, socklen_t *opt_len
) noexcept
{
  return ff_getsockopt(fd, level, opt_name, opt_val, opt_len);
}

std::size_t FStack::read(fd_t fd, void *buf, std::size_t len) noexcept
{
  return ff_read(fd, buf, len);
}

std::size_t FStack::write(fd_t fd, const void *buf, std::size_t len) noexcept
{
  return ff_write(fd, buf, len);
}

// reactor lifecycle

void FStack::init(config_t config_file)
{
  int config_argc = 0;

  char *config_argv[2];

  if (config_file)
  {
    config_argc = 2;

    config_argv[0] = const_cast<char *>("-c");
    config_argv[1] = config_file->data();
  }

  ff_init(config_argc, config_argv);

  _kq = ff_kqueue();
  if (_kq < 0)
  {
    throw std::runtime_error("failed to create kqueue");
  }
}

void FStack::run(loop_func_t loop, void *arg) { ff_run(loop, arg); }

int FStack::poll(event_t events[], std::size_t len) noexcept
{
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = poll_frequency_ms * 1000 * 1000; // NOLINT

  constexpr auto max_int_size_t =
    static_cast<std::size_t>(std::numeric_limits<int>::max());

  // clamp
  len = len < max_int_size_t ? len : max_int_size_t;

  return ff_kevent(_kq, NULL, 0, events, static_cast<int>(len), &ts);
}

void FStack::signal() noexcept
{
  struct kevent signal;
  EV_SET(&signal, KILL_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
  ff_kevent(_kq, &signal, 1, nullptr, 0, nullptr);
}

void FStack::stop() noexcept
{
  // ff_dpdk_stop();

  if (_kq >= 0)
  {
    ff_close(_kq);
    _kq = -1;
  }
}

// events
bool FStack::ev_signal(const event_t &ev) noexcept
{
  return ev.ident == KILL_IDENT && ev.filter == EVFILT_USER;
}

bool FStack::ev_close(const event_t &ev) noexcept
{
  return (ev.flags & EV_EOF) != 0;
}

bool FStack::ev_error(const event_t &ev) noexcept
{
  return (ev.flags & EV_ERROR) != 0;
}

bool FStack::ev_readable(const event_t &ev) noexcept
{
  return ev.filter == EVFILT_READ;
}
bool FStack::ev_writeable(const event_t &ev) noexcept
{
  return ev.filter == EVFILT_WRITE;
}

void *FStack::get_user_data(const event_t &event) noexcept
{
  return event.udata;
}

void FStack::clear(fd_t fd) noexcept
{
  struct kevent kev[2];
  EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  ff_kevent(_kq, kev, 2, NULL, 0, NULL);
}

} // namespace manet::net
