#pragma once

#include <concepts>

#include "manet/protocol/status.hpp"
#include "manet/protocol/websocket_frame.hpp"
#include "manet/reactor/io.hpp"

namespace manet::protocol::websocket
{

template <typename Codec>
concept HasTextHandler = requires { (void)&Codec::on_text; };

template <typename Codec>
concept TextHandler =
  requires(Codec codec, reactor::IO io, std::span<const std::byte> payload) {
    { codec.on_text(io, payload) } noexcept -> std::same_as<Status>;
  };

template <typename Codec>
concept HasBinaryHandler = requires { (void)&Codec::on_binary; };

template <typename Codec>
concept BinaryHandler =
  requires(Codec codec, reactor::IO io, std::span<const std::byte> payload) {
    { codec.on_binary(io, payload) } noexcept -> std::same_as<Status>;
  };

template <typename Codec>
concept HasShutdownHandler = requires { (void)&Codec::on_shutdown; };

template <typename Codec>
concept ShutdownHandler = requires(Codec codec) {
  { codec.on_shutdown() } noexcept -> std::same_as<detail::CloseCode>;
};

template <typename Codec>
concept MessageCodec = (!HasTextHandler<Codec> && TextHandler<Codec>) &&
                       (!HasBinaryHandler<Codec> && BinaryHandler<Codec>) &&
                       (!HasShutdownHandler<Codec> && ShutdownHandler<Codec>);

} // namespace manet::protocol::websocket
