#pragma once

#include <concepts>
#include <optional>

#include "manet/reactor/io.hpp"
#include "manet/transport/status.hpp"

namespace manet::transport
{

template <typename T>
concept HasHandshake = requires { (void)&T::handshake_step; };

template <typename T>
concept Handshake = requires(T::ctx_t &ctx) {
  { T::handshake_step(ctx) } noexcept -> std::same_as<Status>;
};

template <typename T>
concept HasShutdown = requires { (void)&T::shutdown_step; };

template <typename T>
concept Shutdown = requires(T::ctx_t ctx) {
  { T::shutdown_step(ctx) } noexcept -> std::same_as<Status>;
};

template <typename Net, typename T>
concept Transport =
  requires(
    T::ctx_t ctx, typename Net::fd_t fd, T::args_t args,
    manet::reactor::RxSink in, manet::reactor::TxSource out
  ) {
    {
      T::template init<Net>(fd, args)
    } noexcept -> std::same_as<std::optional<typename T::ctx_t>>;
    { T::write(ctx, out) } noexcept -> std::same_as<Status>;
    { T::read(ctx, in) } noexcept -> std::same_as<Status>;
    { T::destroy(ctx) } noexcept -> std::same_as<typename T::ctx_t>;
  } &&
  (!HasHandshake<T> || Handshake<T>) && (!HasShutdown<T> || Shutdown<T>);

} // namespace manet::transport
