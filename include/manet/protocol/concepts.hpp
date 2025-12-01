#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>

#include "manet/protocol/status.hpp"
#include "manet/reactor/io.hpp"

namespace manet::protocol
{

template <typename P>
concept HasConnectHandler = requires { (void)&P::Session::on_connect; };

template <typename P>
concept ConnectHandler = requires(typename P::Session &ctx, reactor::IO io) {
  { ctx.on_connect(io) } noexcept -> std::same_as<Status>;
};

template <typename P>
concept HasHeartbeat = requires { (void)&P::Session::heartbeat; };

template <typename P>
concept Heartbeat = requires(P::Session &ctx, reactor::TxSink output) {
  { ctx.heartbeat(output) } noexcept -> std::same_as<void>;
};

template <typename P>
concept HasShutdown = requires { (void)&P::Session::on_shutdown; };

template <typename P>
concept Shutdown = requires(P::Session &ctx, reactor::IO io) {
  { ctx.on_shutdown(io) } noexcept -> std::same_as<Status>;
};

template <typename P>
concept HasTeardown = requires { (void)&P::Session::teardown; };

template <typename P>
concept Teardown = requires(P::Session &ctx) {
  { ctx.teardown() } noexcept -> std::same_as<Status>;
};

template <typename P>
concept Protocol =
  requires(
    std::string_view host, uint16_t port, typename P::config_t &config,
    typename P::Session &ctx, reactor::IO io
  ) {
    { typename P::Session{host, port, config} } noexcept;
    { ctx.on_data(io) } noexcept -> std::same_as<Status>;
  } &&
  (!HasConnectHandler<P> || ConnectHandler<P>) &&
  (!HasHeartbeat<P> || Heartbeat<P>) && (!HasShutdown<P> || Shutdown<P>) &&
  (!HasTeardown<P> || Teardown<P>);

} // namespace manet::protocol
