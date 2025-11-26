#pragma once

#include <deque>
#include <memory>

#include "manet/reactor/connection.hpp"

namespace manet::transport
{

struct ScriptedTransport
{
  struct script_t
  {
    // handshake script
    std::deque<Status> handshake_results;

    // read script
    std::deque<std::string> read_fragments;
    std::deque<Status> read_status;

    // write script
    std::deque<Status> write_status;

    // what the FSM wrote (share access such that we can retrieve output easily)
    std::shared_ptr<std::string> output = std::make_shared<std::string>();

    // shutdown script
    std::deque<Status> shutdown_results;
  };

  using ctx_t = script_t *;
  using args_t = ctx_t;

  template <typename Net>
  static std::optional<ctx_t> init(int, args_t initial_ctx) noexcept
  {
    if (!initial_ctx)
    {
      return {};
    }

    if (!initial_ctx->output)
    {
      initial_ctx->output = std::make_shared<std::string>();
    }

    return initial_ctx;
  }

  static Status handshake_step(ctx_t &ctx) noexcept
  {
    if (ctx->handshake_results.empty())
      return Status::ok; // default: no handshake

    auto st = ctx->handshake_results.front();
    ctx->handshake_results.pop_front();
    return st;
  }

  static Status read(ctx_t &ctx, reactor::RxSink in) noexcept
  {
    if (ctx->read_status.empty())
      return Status::close; // default: EOF

    auto st = ctx->read_status.front();
    ctx->read_status.pop_front();

    if (st == Status::ok)
    {
      if (ctx->read_fragments.empty())
        return Status::error; // script bug

      auto &chunk = ctx->read_fragments.front();
      auto n = std::min<std::size_t>(chunk.size(), in.wbuf().size());
      std::memcpy(in.wbuf().data(), chunk.data(), n);
      in.wrote(n);
      chunk.erase(0, n);
      if (chunk.empty())
        ctx->read_fragments.pop_front();
    }

    return st;
  }

  static Status write(ctx_t &ctx, reactor::TxSource out) noexcept
  {
    if (ctx->write_status.empty())
    {
      // by default, accept and record everything
      ctx->output.get()->append(
        reinterpret_cast<const char *>(out.rbuf().data()), out.rbuf().size()
      );
      out.read(out.rbuf().size());
      return Status::ok;
    }

    auto st = ctx->write_status.front();
    ctx->write_status.pop_front();

    if (st == Status::ok)
    {
      ctx->output.get()->append(
        reinterpret_cast<const char *>(out.rbuf().data()), out.rbuf().size()
      );
      out.read(out.rbuf().size());
    }

    return st;
  }

  static Status shutdown_step(ctx_t &ctx) noexcept
  {
    if (ctx->shutdown_results.empty())
      return Status::ok;

    auto st = ctx->shutdown_results.front();
    ctx->shutdown_results.pop_front();
    return st;
  }

  static ctx_t destroy(ctx_t &ctx) noexcept { return ctx; }
};

inline ScriptedTransport::script_t transport_happypath(
  std::initializer_list<std::string_view> fragments,
  std::initializer_list<Status> handshake = {},
  std::initializer_list<Status> write_status = {},
  std::initializer_list<Status> shutdown = {}
)
{
  ScriptedTransport::script_t script{};

  for (auto st : handshake)
  {
    script.handshake_results.push_back(st);
  }

  for (auto fragment : fragments)
  {
    script.read_fragments.emplace_back(fragment);

    // each fragment should return one Status::ok
    script.read_status.push_back(Status::ok);
  }

  // close
  script.read_status.push_back(Status::close);

  // writes and shutdowns:
  for (auto st : write_status)
    script.write_status.push_back(st);
  for (auto st : shutdown)
    script.shutdown_results.push_back(st);

  return script;
}

} // namespace manet::transport
