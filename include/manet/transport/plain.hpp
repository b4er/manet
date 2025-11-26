#pragma once

#include <optional>
#include <variant>

#include "manet/reactor/io.hpp"
#include "status.hpp"

namespace manet::transport
{

struct Plain
{
  using args_t = std::monostate;

  struct ctx_t
  {
    int fd;
    int (*net_read)(int, void *, std::size_t) noexcept;
    int (*net_write)(int, const void *, std::size_t) noexcept;
  };

  template <typename Net>
  static std::optional<ctx_t> init(int fd, args_t) noexcept
  {
    return ctx_t{
      .fd = fd,
      .net_read = +[](int fd, void *ptr, std::size_t n) noexcept
                  { return Net::read(fd, ptr, n); },
      .net_write = +[](int fd, const void *ptr, std::size_t n) noexcept
                   { return Net::write(fd, ptr, n); }
    };
  }

  static Status write(ctx_t &ctx, reactor::TxSource out) noexcept;
  static Status read(ctx_t &ctx, reactor::RxSink in) noexcept;

  static ctx_t destroy(ctx_t) noexcept;
};

} // namespace manet::transport
