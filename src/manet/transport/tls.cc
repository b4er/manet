#include "manet/transport/tls.hpp"
#include "manet/transport/status.hpp"

namespace manet::transport::tls_detail
{

Status TlsEndpoint::handshake_step() noexcept
{
  int r = SSL_connect(ssl_ctx);
  if (r != 1)
  {
    int err = SSL_get_error(ssl_ctx, r);
    if (err == SSL_ERROR_WANT_WRITE)
    {
      return Status::want_write;
    }
    else if (err == SSL_ERROR_WANT_READ)
    {
      return Status::want_read;
    }
    else
    {
      if constexpr (log::enabled)
      {
        unsigned long e = ERR_get_error();
        log::error(
          "SSL_connect failed ({}): {}", r, ERR_error_string(e, nullptr)
        );
      }
      return Status::error;
    }
  }

  return Status::ok;
}

Status TlsEndpoint::read(reactor::RxSink in) noexcept
{
  constexpr auto max_int_size_t =
    static_cast<std::size_t>(std::numeric_limits<int>::max());

  // clamp
  const auto sz =
    in.wbuf().size() < max_int_size_t ? in.wbuf().size() : max_int_size_t;

  const int len = SSL_read(ssl_ctx, in.wbuf().data(), static_cast<int>(sz));
  if (len > 0)
  {
    in.wrote(len);
    return Status::ok;
  }

  const int err = SSL_get_error(ssl_ctx, len);
  return err == SSL_ERROR_WANT_READ     ? Status::want_read
         : err == SSL_ERROR_WANT_WRITE  ? Status::want_write
         : err == SSL_ERROR_ZERO_RETURN ? Status::close
                                        : Status::error;
}

Status TlsEndpoint::write(reactor::TxSource out) noexcept
{
  constexpr auto max_int_size_t =
    static_cast<std::size_t>(std::numeric_limits<int>::max());

  const auto sz =
    out.rbuf().size() < max_int_size_t ? out.rbuf().size() : max_int_size_t;

  const int len = SSL_write(ssl_ctx, out.rbuf().data(), static_cast<int>(sz));
  if (len > 0)
  {
    out.read(len);
    return Status::ok;
  }

  const int err = SSL_get_error(ssl_ctx, len);
  return err == SSL_ERROR_WANT_READ     ? Status::want_read
         : err == SSL_ERROR_WANT_WRITE  ? Status::want_write
         : err == SSL_ERROR_ZERO_RETURN ? Status::close
                                        : Status::error;
}

Status TlsEndpoint::shutdown_step() noexcept
{
  int r = SSL_shutdown(ssl_ctx);

  // we both are done
  if (r == 1)
  {
    return Status::ok;
  }

  /* // half-closed: we close_notify the server but have not heard back
  if (r == 0)
  {
    int w = SSL_want(ctx);
    bool write = w & SSL_WRITING;
    // SSL_shutdown just sent close_notify, so probably we want READ
    return write ? Status::want_write : Status::want_read;
  } */

  int e = SSL_get_error(ssl_ctx, r);

  if (e == SSL_ERROR_ZERO_RETURN)
  {
    return Status::ok;
  }

  if (e == SSL_ERROR_WANT_WRITE)
  {
    return Status::want_write;
  }

  if (e == SSL_ERROR_WANT_READ)
  {
    return Status::want_read;
  }

  return Status::error;
}

void TlsEndpoint::destroy() noexcept
{
  if (ssl_ctx)
  {
    SSL_free(ssl_ctx);
  }
}

} // namespace manet::transport::tls_detail
