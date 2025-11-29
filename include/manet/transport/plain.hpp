#pragma once

#include <optional>
#include <variant>

#include "manet/reactor/io.hpp"
#include "status.hpp"

namespace manet::transport
{

struct Plain
{
  using config_t = std::monostate;

  template <typename Net> struct Endpoint
  {
    using fd_t = typename Net::fd_t;

    fd_t fd;

    std::size_t (*net_read)(fd_t, void *, std::size_t) noexcept;
    std::size_t (*net_write)(fd_t, const void *, std::size_t) noexcept;

    static std::optional<Endpoint> init(fd_t fd, config_t) noexcept
    {
      return Endpoint{
        .fd = fd,
        .net_read = +[](fd_t fd, void *ptr, std::size_t n) noexcept
                    { return Net::read(fd, ptr, n); },
        .net_write = +[](fd_t fd, const void *ptr, std::size_t n) noexcept
                     { return Net::write(fd, ptr, n); }
      };
    }

    Status read(reactor::RxSink rx) noexcept
    {
      while (true)
      {
        int len = net_read(fd, rx.wbuf().data(), rx.wbuf().size());
        if (len > 0)
        {
          rx.wrote(len);
          return Status::ok;
        }

        if (len == 0)
        {
          return Status::close;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return Status::want_read;
        }

        if (errno == EINTR)
        {
          continue;
        }

        return Status::error;
      }
    }

    Status write(reactor::TxSource tx) noexcept
    {
      while (true)
      {
        int len = net_write(fd, tx.rbuf().data(), tx.rbuf().size());
        if (len >= 0)
        {
          tx.read(len);
          return Status::ok;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return Status::want_write;
        }

        if (errno == EINTR)
        {
          continue;
        }

        return Status::error;
      }
    }

    void destroy() noexcept {} // no-op
  };
};

} // namespace manet::transport
