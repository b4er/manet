#pragma once

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "manet/reactor/io.hpp"
#include "manet/utils/logging.hpp"
#include "status.hpp"
#include "tls_bio.hpp"

namespace manet::transport
{

namespace tls_detail
{

struct TlsEndpoint
{
  SSL *ssl_ctx;

  Status handshake_step() noexcept;
  Status read(reactor::RxSink rx) noexcept;
  Status write(reactor::TxSource tx) noexcept;
  Status shutdown_step() noexcept;
  void destroy() noexcept;
};

} // namespace tls_detail

/** assumed to be used with single Net (such as PosixNet, or FStackNet) */
struct Tls
{
  using config_t = const char *;

  template <typename Net> struct Endpoint : tls_detail::TlsEndpoint
  {
    using fd_t = typename Net::fd_t;

    static std::optional<Endpoint> init(fd_t fd, const config_t &host) noexcept
    {
      tls_detail::g_tls_init<Net>();

      auto ctx = SSL_new(tls_detail::g_tls_ctx);
      if (!ctx)
      {
        if constexpr (utils::logging_enabled)
        {
          ERR_print_errors_fp(stderr);
          utils::error("cannot create SSL");
        }
        return {};
      }

      BIO *bio = socket_BIO<Net>(fd);
      SSL_set_bio(ctx, bio, bio);
      SSL_set_connect_state(ctx);

      /* verify host name: */

      if (SSL_set_tlsext_host_name(ctx, host) != 1)
      {
        if constexpr (utils::logging_enabled)
        {
          unsigned long e = ERR_get_error();
          utils::error("SNI set failed: {}", ERR_error_string(e, nullptr));
        }

        SSL_free(ctx);
        return {};
      }

      SSL_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

      if (SSL_set1_host(ctx, host) != 1)
      {
        if constexpr (utils::logging_enabled)
        {
          unsigned long e = ERR_get_error();
          utils::error(
            "SSL_set1_host failed: {}", ERR_error_string(e, nullptr)
          );
        }
        SSL_free(ctx);
        return {};
      }

      return Endpoint{.ssl_ctx = ctx};
    }
  };
};

} // namespace manet::transport
