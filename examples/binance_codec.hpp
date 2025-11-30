#pragma once

// #include <sbepp/sbepp.hpp>

#include <binance_sbe/binance_sbe.hpp>

#include <manet/logging.hpp>
#include <manet/protocol/status.hpp>
#include <manet/reactor/io.hpp>

namespace manet::protocol::websocket
{

struct BinanceDepth
{
  using config_t = std::monostate;
  BinanceDepth(config_t) {}

  Status on_binary(reactor::TxSink, std::span<const std::byte> payload) noexcept
  {
    auto header = sbepp::make_const_view<
      sbepp::schema_traits<binance_sbe::schema>::header_type>(
      payload.data(), payload.size()
    );

    if (header.templateId() ==
        sbepp::message_traits<
          binance_sbe::schema::messages::BestBidAskStreamEvent>::id())
    {
      log::trace("BestBidAskStreamEvent");
    }
    else if (header.templateId() ==
             sbepp::message_traits<
               binance_sbe::schema::messages::DepthDiffStreamEvent>::id())
    {
      log::trace("DepthDiffStreamEvent");
    }
    else if (header.templateId() ==
             sbepp::message_traits<
               binance_sbe::schema::messages::DepthSnapshotStreamEvent>::id())
    {
      log::trace("DepthSnapshotStreamEvent");
    }
    else if (header.templateId() ==
             sbepp::message_traits<
               binance_sbe::schema::messages::TradesStreamEvent>::id())
    {
      log::trace("TradesStreamEvent");
    }
    else
    {
      return Status::error;
    }

    return Status::ok;
  }
};

} // namespace manet::protocol::websocket
