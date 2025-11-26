#include "manet/transport/tls_bio.hpp"

namespace manet::transport::tls_detail
{

int bio_create(BIO *bio)
{
  BIO_set_init(bio, 1);
  BIO_set_data(bio, new SocketBioData{-1});
  return 1;
}

int bio_destroy(BIO *bio)
{
  if (!bio)
  {
    return 0;
  }
  delete static_cast<SocketBioData *>(BIO_get_data(bio));
  BIO_set_data(bio, nullptr);
  BIO_set_init(bio, 0);
  return 1;
}

long bio_ctrl(BIO *bio, int cmd, long, void *ptr)
{
  auto *data = static_cast<SocketBioData *>(BIO_get_data(bio));

  switch (cmd)
  {
  case BIO_C_SET_FD:
  {
    data->fd = ptr ? *static_cast<int *>(ptr) : -1;
    return 1;
  }
  case BIO_C_GET_FD:
  {
    if (ptr)
      *static_cast<int *>(ptr) = data->fd;
    return data->fd;
  }
  case BIO_CTRL_FLUSH:
  {
    return 1;
  }
  case BIO_CTRL_PENDING:
  case BIO_CTRL_WPENDING:
  {
    return 0;
  }
  default:
  {
    return 0;
  }
  }
}

} // namespace manet::transport::tls_detail
