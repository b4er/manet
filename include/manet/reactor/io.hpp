#pragma once

#include "buffer.hpp"

namespace manet::reactor
{

static constexpr std::size_t RX_CAP = 1 << 20;
static constexpr std::size_t TX_CAP = 1 << 20;

template <std::size_t CAP> struct Input
{
  Buffer<CAP> *rx;
  auto rbuf() const { return rx->rbuf(); }
  void read(std::size_t len) { rx->inc_rpos(len); }
};

template <std::size_t CAP> struct Output
{
  Buffer<CAP> *tx;
  auto wbuf() const { return tx->wbuf(); }
  void wrote(std::size_t len) { tx->inc_wpos(len); }
};

using RxSource = Input<RX_CAP>;
using RxSink = Output<RX_CAP>;

using TxSource = Input<TX_CAP>;
using TxSink = Output<TX_CAP>;

struct IO : RxSource, TxSink
{
};

} // namespace manet::reactor
