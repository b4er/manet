#pragma once

#include <concepts>
#include <cstddef>
#include <sys/socket.h>
#include <sys/types.h>
#include <utility>

namespace manet::net
{

constexpr const int poll_frequency_ms = 100;

template <typename Backend>
concept Net = requires(
  typename Backend::config_t config, typename Backend::fd_t fd,
  typename Backend::event_t ev,

  int domain, int type, int proto, long req, void *argp, const void *sa,
  socklen_t l, int level, int opt_name, void *opt_val, socklen_t *opt_len,
  int argc, char *argv[], int (*loop)(void *), void *arg,
  typename Backend::event_t events[], std::size_t len, void *ptr,
  const void *cptr, bool b
) {
  { Backend::name } -> std::convertible_to<const char *>;

  {
    Backend::socket(domain, type, proto)
  } noexcept -> std::same_as<typename Backend::fd_t>;
  { Backend::ioctl(fd, req, argp) } noexcept -> std::same_as<int>;
  { Backend::connect(fd, sa, l) } noexcept -> std::same_as<int>;
  { Backend::close(fd) } noexcept -> std::same_as<int>;

  {
    Backend::getsockopt(fd, level, opt_name, opt_val, opt_len)
  } noexcept -> std::same_as<int>;

  { Backend::init(config) } -> std::same_as<void>;
  { Backend::run(loop, arg) };

  { Backend::poll(events, len) } noexcept -> std::same_as<int>;

  { Backend::signal() } noexcept;
  { Backend::stop() } noexcept;

  { Backend::ev_signal(std::as_const(ev)) } noexcept -> std::same_as<bool>;
  { Backend::ev_close(std::as_const(ev)) } noexcept -> std::same_as<bool>;
  { Backend::ev_error(std::as_const(ev)) } noexcept -> std::same_as<bool>;
  { Backend::ev_readable(std::as_const(ev)) } noexcept -> std::same_as<bool>;
  { Backend::ev_writeable(std::as_const(ev)) } noexcept -> std::same_as<bool>;

  { Backend::get_user_data(ev) } noexcept -> std::same_as<void *>;

  { Backend::subscribe(ptr, fd, true, false) } noexcept;
  { Backend::subscribe(ptr, fd, false, true) } noexcept;
  { Backend::subscribe(ptr, fd, true, true) } noexcept;

  { Backend::read(fd, ptr, len) } noexcept -> std::same_as<ssize_t>;
  { Backend::write(fd, cptr, len) } noexcept -> std::same_as<ssize_t>;

  { Backend::clear(fd) } noexcept;
};

} // namespace manet::net
