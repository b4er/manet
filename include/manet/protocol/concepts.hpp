#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>

#include "manet/protocol/status.hpp"
#include "manet/reactor/io.hpp"

namespace manet::protocol
{

template <typename P>
concept HasConnectHandler = requires { (void)&P::on_connect; };

template <typename P>
concept ConnectHandler =
  requires(P::ctx_t &ctx, manet::reactor::TxSink output) {
    { P::on_connect(ctx, output) } noexcept -> std::same_as<Status>;
  };

template <typename P>
concept HasHeartbeat = requires { (void)&P::heartbeat; };

template <typename P>
concept Heartbeat = requires(P::ctx_t &ctx, manet::reactor::TxSink output) {
  { P::heartbeat(ctx, output) } noexcept -> std::same_as<void>;
};

template <typename P>
concept HasShutdown = requires { (void)&P::on_shutdown; };

template <typename P>
concept Shutdown = requires(P::ctx_t &ctx, manet::reactor::IO io) {
  { P::on_shutdown(ctx, io) } noexcept -> std::same_as<Status>;
};

template <typename P>
concept HasTeardown = requires { (void)&P::teardown; };

template <typename P>
concept Teardown = requires(P::ctx_t &ctx) {
  { P::teardown(ctx) } noexcept -> std::same_as<Status>;
};

template <typename P>
concept Protocol =
  requires(
    std::string_view host, uint16_t port, const typename P::args_t args,
    P::ctx_t &ctx, manet::reactor::IO io, manet::reactor::TxSink out
  ) {
    { P::init(host, port, args) } noexcept -> std::same_as<typename P::ctx_t>;
    { P::on_data(ctx, io) } noexcept -> std::same_as<Status>;
  } &&
  (!HasConnectHandler<P> || ConnectHandler<P>) &&
  (!HasHeartbeat<P> || Heartbeat<P>) && (!HasShutdown<P> || Shutdown<P>) &&
  (!HasTeardown<P> || Teardown<P>);

} // namespace manet::protocol
