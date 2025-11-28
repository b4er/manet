#pragma once

#include <deque>
#include <memory>

#include "manet/reactor/connection.hpp"
#include "manet/transport/status.hpp"

struct ScriptedTransport
{
  struct script_t
  {
    // handshake script
    std::deque<manet::transport::Status> handshake_results;

    // read script
    std::deque<std::string> read_fragments;
    std::deque<manet::transport::Status> read_status;

    // write script
    std::deque<manet::transport::Status> write_status;

    // what the FSM wrote (share access such that we can retrieve output easily)
    std::shared_ptr<std::string> output = std::make_shared<std::string>();

    // shutdown script
    std::deque<manet::transport::Status> shutdown_results;
  };

  using config_t = script_t *;

  template <typename Net> struct Endpoint
  {
    using fd_t = int;
    script_t *script;

    static std::optional<Endpoint> init(int, config_t script) noexcept
    {
      if (!script)
      {
        return {};
      }

      if (!script->output)
      {
        script->output = std::make_shared<std::string>();
      }

      return Endpoint{script};
    }

    manet::transport::Status handshake_step() noexcept
    {
      if (script->handshake_results.empty())
        return manet::transport::Status::ok; // default: no handshake

      auto st = script->handshake_results.front();
      script->handshake_results.pop_front();
      return st;
    }

    manet::transport::Status read(manet::reactor::RxSink in) noexcept
    {
      if (script->read_status.empty())
        return manet::transport::Status::close; // default: EOF

      auto st = script->read_status.front();
      script->read_status.pop_front();

      if (st == manet::transport::Status::ok)
      {
        if (script->read_fragments.empty())
          return manet::transport::Status::error; // script bug

        auto &chunk = script->read_fragments.front();
        auto n = std::min<std::size_t>(chunk.size(), in.wbuf().size());
        std::memcpy(in.wbuf().data(), chunk.data(), n);
        in.wrote(n);
        chunk.erase(0, n);
        if (chunk.empty())
          script->read_fragments.pop_front();
      }

      return st;
    }

    manet::transport::Status write(manet::reactor::TxSource out) noexcept
    {
      if (script->write_status.empty())
      {
        // by default, accept and record everything
        script->output.get()->append(
          reinterpret_cast<const char *>(out.rbuf().data()), out.rbuf().size()
        );
        out.read(out.rbuf().size());
        return manet::transport::Status::ok;
      }

      auto st = script->write_status.front();
      script->write_status.pop_front();

      if (st == manet::transport::Status::ok)
      {
        script->output.get()->append(
          reinterpret_cast<const char *>(out.rbuf().data()), out.rbuf().size()
        );
        out.read(out.rbuf().size());
      }

      return st;
    }

    manet::transport::Status shutdown_step() noexcept
    {
      if (script->shutdown_results.empty())
        return manet::transport::Status::ok;

      auto st = script->shutdown_results.front();
      script->shutdown_results.pop_front();
      return st;
    }

    void destroy() noexcept {}
  };
};

inline ScriptedTransport::script_t happypath(
  std::initializer_list<std::string_view> fragments,
  std::initializer_list<manet::transport::Status> handshake = {},
  std::initializer_list<manet::transport::Status> write_status = {},
  std::initializer_list<manet::transport::Status> shutdown = {}
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

    // each fragment should return one manet::transport::Status::ok
    script.read_status.push_back(manet::transport::Status::ok);
  }

  // close
  script.read_status.push_back(manet::transport::Status::close);

  // writes and shutdowns:
  for (auto st : write_status)
    script.write_status.push_back(st);
  for (auto st : shutdown)
    script.shutdown_results.push_back(st);

  return script;
}
