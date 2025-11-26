#include <sys/socket.h>

#include "manet/transport/plain.hpp"

namespace manet::transport
{

Status Plain::write(ctx_t &ctx, manet::reactor::TxSource out) noexcept
{
  while (true)
  {
    int len = ctx.net_write(ctx.fd, out.rbuf().data(), out.rbuf().size());
    if (len >= 0)
    {
      out.read(len);
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

Status Plain::read(ctx_t &ctx, manet::reactor::RxSink in) noexcept
{
  while (true)
  {
    int len = ctx.net_read(ctx.fd, in.wbuf().data(), in.wbuf().size());
    if (len > 0)
    {
      in.wrote(len);
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

// we do not own the socket_fd
Plain::ctx_t Plain::destroy(ctx_t ctx) noexcept { return ctx; }

} // namespace manet::transport
