#pragma once

#include <binance_sbe/binance_sbe.hpp>

#include <manet/logging.hpp>
#include <manet/protocol/status.hpp>
#include <manet/reactor/io.hpp>

#include <rigtorp/SPSCQueue.h>

enum class Side : uint8_t
{
  bid,
  ask
};

namespace binance
{

enum class Symbol : uint8_t
{
  BTC
};

/** per-lever market depth diff */
struct DepthEvent
{
  Symbol symbol;
  Side side;
  int64_t event_time_ns; // remote time
  int64_t recv_time_ns;  // local time
  int64_t update_id;

  int64_t price_exp;
  int64_t price;

  int64_t qty_exp;
  int64_t qty;
};

namespace log = manet::log;

/** WebSocket codec for Binance SBE depth stream: parse DepthDiffStreamEvent
 * messages and enqueue them per-level (as DepthEvent records) into spsc
 * queuspsc queue.
 */
struct BinanceDepth
{
  using Status = manet::protocol::Status;

  using config_t = rigtorp::SPSCQueue<DepthEvent> *;

  explicit BinanceDepth(config_t queue) noexcept
      : _queue(queue)
  {
  }

  Status
  on_binary(manet::reactor::TxSink, std::span<const std::byte> payload) noexcept
  {
    constexpr auto DepthDiffEventId = sbepp::message_traits<
      binance_sbe::schema::messages::DepthDiffStreamEvent>::id();

    auto header = sbepp::make_const_view<
      sbepp::schema_traits<binance_sbe::schema>::header_type>(
      payload.data(), payload.size()
    );

    if (header.templateId() == DepthDiffEventId)
    {
      auto diff =
        sbepp::make_const_view<binance_sbe::messages::DepthDiffStreamEvent>(
          payload.data(), payload.size()
        );

      auto size_check = sbepp::size_bytes_checked(diff, payload.size());
      if (!size_check.valid)
      {
        log::error("bad DepthDiffStreamEvent message");
        return Status::error;
      }

      return push_diff(diff);
    }
    else if (header.templateId() ==
             sbepp::message_traits<
               binance_sbe::schema::messages::BestBidAskStreamEvent>::id())
    {
      log::trace("dropping BestBidAskStreamEvent");
      return Status::ok;
    }
    else if (header.templateId() ==
             sbepp::message_traits<
               binance_sbe::schema::messages::DepthSnapshotStreamEvent>::id())
    {
      log::trace("dropping DepthSnapshotStreamEvent");
      return Status::ok;
    }
    else if (header.templateId() ==
             sbepp::message_traits<
               binance_sbe::schema::messages::TradesStreamEvent>::id())
    {
      log::info("dropping TradesStreamEvent");
      return Status::ok;
    }
    else
    {
      return Status::error;
    }
  }

private:
  rigtorp::SPSCQueue<DepthEvent> *_queue;
  Status push_diff(
    binance_sbe::messages::DepthDiffStreamEvent<const std::byte> diff
  ) noexcept
  {
    const auto recv_time_ns = 0;

    for (auto ask : diff.asks())
    {
      if (!_queue->try_push(
            DepthEvent{
              .symbol = Symbol::BTC,
              .side = Side::ask,
              .event_time_ns = diff.eventTime().value(),
              .recv_time_ns = recv_time_ns,
              .update_id = diff.lastBookUpdateId().value(),

              .price_exp = diff.priceExponent().value(),
              .price = ask.price().value(),

              .qty_exp = diff.qtyExponent().value(),
              .qty = ask.qty().value(),
            }
          ))
      {
        log::warn("pushing binance::DepthEvent failed (queue full)");
        return Status::ok;
      }
    }

    for (auto bid : diff.bids())
    {
      if (!_queue->try_push(
            DepthEvent{
              .symbol = Symbol::BTC,
              .side = Side::bid,
              .event_time_ns = diff.eventTime().value(),
              .recv_time_ns = recv_time_ns,
              .update_id = diff.lastBookUpdateId().value(),

              .price_exp = diff.priceExponent().value(),
              .price = bid.price().value(),

              .qty_exp = diff.qtyExponent().value(),
              .qty = bid.qty().value(),
            }
          ))
      {
        log::warn("pushing binance::DepthEvent failed (queue full)");
        return Status::ok;
      }
    }

    return Status::ok;
  }
};

} // namespace binance
