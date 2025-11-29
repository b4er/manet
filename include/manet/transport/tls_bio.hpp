#pragma once

#include <mutex>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "manet/logging.hpp"
#include "manet/reactor/io.hpp"
#include "status.hpp"

#if defined(LIBRESSL_VERSION_NUMBER)
#error "LibreSSL not supported"
#elif OPENSSL_VERSION_NUMBER < 0x10101000L
#error "OpenSSL >= 1.1.1 required"
#endif

namespace manet::transport::tls::detail
{

inline SSL_CTX *g_tls_ctx = nullptr;

struct SocketBioData
{
  int fd;
};

int bio_create(BIO *bio);
long bio_ctrl(BIO *bio, int cmd, long, void *);
int bio_destroy(BIO *bio);

template <typename Net> static int bio_read(BIO *bio, char *out, int outl)
{
  auto *data = static_cast<SocketBioData *>(BIO_get_data(bio));
  BIO_clear_retry_flags(bio);

  while (true)
  {
    int n = Net::read(data->fd, out, outl);

    if (n >= 0)
    {
      return n;
    }

    if (errno == EINTR)
    {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
      BIO_set_retry_read(bio);
    }

    return -1;
  }
}

template <typename Net> static int bio_write(BIO *bio, const char *in, int inl)
{
  auto *data = static_cast<SocketBioData *>(BIO_get_data(bio));

  if (!in || inl <= 0)
  {
    return 0;
  }

  BIO_clear_retry_flags(bio);

  while (true)
  {
    int n = Net::write(data->fd, in, inl);
    if (n >= 0)
    {
      return n;
    }

    if (errno == EINTR)
      continue;

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
      BIO_set_retry_write(bio);
    }

    return -1;
  }
}

template <typename Net> BIO_METHOD *BIO_method()
{
  static BIO_METHOD *meth = []
  {
    BIO_METHOD *m = BIO_meth_new(BIO_TYPE_SOURCE_SINK, Net::name);
    BIO_meth_set_create(m, &bio_create);
    BIO_meth_set_ctrl(m, &bio_ctrl);
    BIO_meth_set_destroy(m, &bio_destroy);
    BIO_meth_set_read(m, &bio_read<Net>);
    BIO_meth_set_write(m, &bio_write<Net>);
    return m;
  }();

  return meth;
}

template <typename Net> BIO *socket_BIO(int fd)
{
  BIO *bio = BIO_new(BIO_method<Net>());

  auto *data = static_cast<SocketBioData *>(BIO_get_data(bio));
  data->fd = fd;

  return bio;
}

template <typename Net> inline void g_tls_free()
{
  if (g_tls_ctx)
  {
    SSL_CTX_free(g_tls_ctx);
    g_tls_ctx = nullptr;
  }

  BIO_METHOD *meth = BIO_method<Net>();
  if (meth)
  {
    BIO_meth_free(meth);
  }

  OPENSSL_cleanup();
}

/** this should not be called twice with different Net (normal) */
template <typename Net> inline void g_tls_init() noexcept
{
  static std::once_flag once;

  std::call_once(
    once,
    []
    {
      SSL_library_init();
      OpenSSL_add_all_algorithms();
      SSL_load_error_strings();

      g_tls_ctx = SSL_CTX_new(TLS_client_method());
      if (!g_tls_ctx)
      {
        throw std::runtime_error("SSL_CTX_new error");
      }

      // trust store:
      if (SSL_CTX_set_default_verify_paths(g_tls_ctx) != 1)
      {
        throw std::runtime_error("SSL_CTX_set_default_verify_paths error");
      }

      // require at least TLS 1.2
      SSL_CTX_set_min_proto_version(g_tls_ctx, TLS1_2_VERSION);

      std::atexit(g_tls_free<Net>);
    }
  );
}

} // namespace manet::transport::tls::detail
