#pragma once

#include <concepts>
#include <optional>

#include "manet/reactor/io.hpp"
#include "manet/transport/status.hpp"

namespace manet::transport
{

template <typename Net, typename T>
concept HasHandshake =
  requires { (void)&T::template Endpoint<Net>::handshake_step; };

template <typename Net, typename T>
concept Handshake = requires(typename T::template Endpoint<Net> &ctx) {
  { ctx.handshake_step() } -> std::same_as<transport::Status>;
};

template <typename Net, typename T>
concept HasShutdown =
  requires { (void)&T::template Endpoint<Net>::shutdown_step; };

template <typename Net, typename T>
concept Shutdown = requires(typename T::template Endpoint<Net> &ctx) {
  { ctx.shutdown_step() } -> std::same_as<transport::Status>;
};

template <typename Net, typename T>
concept Transport =
  requires(
    typename T::template Endpoint<Net> &ctx, typename Net::fd_t fd,
    const typename T::config_t &config, manet::reactor::RxSink in,
    manet::reactor::TxSource out
  ) {
    {
      T::template Endpoint<Net>::init(fd, config)
    } noexcept
      -> std::same_as<std::optional<typename T::template Endpoint<Net>>>;
    { ctx.write(out) } noexcept -> std::same_as<Status>;
    { ctx.read(in) } noexcept -> std::same_as<Status>;
    { ctx.destroy() } noexcept -> std::same_as<void>;
  } &&
  (!HasHandshake<Net, T> || Handshake<Net, T>) &&
  (!HasShutdown<Net, T> || Shutdown<Net, T>);

} // namespace manet::transport
