#pragma once

#include <cassert>
#include <cstring>
#include <deque>
#include <errno.h>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace manet::net
{

namespace test
{

struct FdAction
{
  enum class kind_t
  {
    read_quota,
    write_quota
  } kind;
  std::size_t quota = 0;

  static FdAction GrantRead(std::size_t len)
  {
    return {kind_t::read_quota, len};
  }

  static FdAction GrantWrite(std::size_t len)
  {
    return {kind_t::write_quota, len};
  }
};

struct FdScript
{
  std::deque<FdAction> actions;
  enum class sentinel_t
  {
    CONNRESET,
    HUP,
  } sentinel;

  std::span<const std::byte> input;

  bool connect_async;
};

struct FdState
{
  // client data / mask
  void *user_data = nullptr;

  bool want_read = false;
  bool want_write = false;

  // flags
  bool connected = false;
  bool hup = false;
  bool err = false;

  bool delivered_hup = false;
  bool delivered_err = false;

  std::size_t rquota = 0;
  std::size_t wquota = 0;

  bool winprogress = false;

  bool prev_read_ready = false;
  bool prev_write_ready = false;

  // test script & output buffer
  FdScript script;
};

} // namespace test

struct TestNet
{
  using config_t = std::deque<test::FdScript>;
  using fd_t = int;

  static inline bool _alive = true;

  struct event_t
  {
    bool readable = false;
    bool writeable = false;

    bool signal = false;
    bool close = false;
    bool error = false;

    void *user_data = nullptr;
  };

  static constexpr const char *name = "test";

  // sockets
  static fd_t socket(int, int, int) noexcept
  {
    // we need a script to attach to the fd
    if (_scripts.empty())
    {
      errno = ENOBUFS;
      return -1;
    }

    fd_t fd = _next_fd++;

    _sockets[fd] = test::FdState{.script = std::move(_scripts.front())};
    _outputs[fd] = {};

    _scripts.pop_front();

    return fd;
  }

  static int ioctl(fd_t, long, void *) noexcept { return 0; }

  static int connect(fd_t fd, const void *sa, socklen_t l) noexcept
  {
    if (!_sockets.contains(fd))
    {
      errno = ENOTSOCK;
      return -1;
    }

    if (_sockets.contains(fd) && _sockets[fd].connected)
    {
      errno = EISCONN;
      return -1;
    }

    auto &socket = _sockets[fd];

    socket.connected = true;

    // simulate asynchronous connect
    if (socket.script.connect_async)
    {
      socket.winprogress = true;
      errno = EINPROGRESS;
      return -1;
    }

    return 0;
  }

  static int close(fd_t fd) noexcept
  {
    if (_sockets.contains(fd))
    {
      _sockets.erase(fd);
      return 0;
    }
    else
    {
      errno = EBADF;
      return -1;
    }
  }

  static int getsockopt(
    fd_t fd, int level, int opt_name, void *opt_val, socklen_t *opt_len
  ) noexcept
  {
    return 0;
  }

  static void apply() noexcept {}

  static int read(fd_t fd, void *ptr, std::size_t len) noexcept
  {
    if (!_sockets.contains(fd))
    {
      errno = EBADF;
      return -1;
    }

    auto &socket = _sockets[fd];

    auto consumed = std::min({socket.rquota, len, socket.script.input.size()});
    if (consumed == 0)
    {
      errno = EAGAIN;
      return -1;
    }

    // copy & consume the bytes:
    std::memcpy(ptr, socket.script.input.data(), consumed);

    socket.script.input = socket.script.input.subspan(consumed);
    socket.rquota -= consumed;

    return consumed;
  }

  static int write(fd_t fd, const void *ptr, std::size_t len) noexcept
  {
    if (!_sockets.contains(fd) || !_outputs.contains(fd))
    {
      errno = EBADF;
      return -1;
    }

    auto &socket = _sockets[fd];
    auto &output = _outputs[fd];

    auto consumed = std::min(socket.wquota, len);
    if (consumed == 0)
    {
      errno = EAGAIN; // NEW: backpressure
      return -1;
    }

    auto p = static_cast<const std::byte *>(ptr);
    output.insert(output.end(), p, p + consumed);
    socket.wquota -= consumed;

    return consumed;
  }

  // reactor lifecycle
  static void init(config_t config)
  {
    // due to re-using the static Net, these need to completely reset:
    _alive = true;

    _scripts = config;
    _next_fd = 0;
    _signals = 0;
    _sockets = {};
    _outputs = {};
  }

  static void run(int (*loop)(void *arg), void *arg)
  {
    while (_alive && _sockets.size())
    {
      loop(arg);
    }
  }

  static void signal() noexcept { _signals++; }

  static void stop() noexcept { _alive = false; }

  static int poll(event_t events[], std::size_t len) noexcept
  {
    assert(len == _sockets.size() + 1); //  avoid fairness issues

    std::size_t i = 0;

    if (_signals)
    {
      events[i++] = {.signal = true};
      _signals--;
    }

    for (auto it = _sockets.begin(); it != _sockets.end();)
    {
      auto &[fd, state] = *it;

      if (!state.err && !state.hup)
      {
        // advance the script action (or sentinel)
        if (state.script.actions.empty())
        {
          // script is done: terminate with HUP or ERR
          switch (state.script.sentinel)
          {
          case test::FdScript::sentinel_t::HUP:
            state.hup = true;
            break;
          case test::FdScript::sentinel_t::CONNRESET:
            state.err = true;
            break;
          }
        }
        else // grant quota (read / write)
        {
          auto action = state.script.actions.front();
          state.script.actions.pop_front();

          switch (action.kind)
          {
          case test::FdAction::kind_t::read_quota:
            state.rquota += action.quota;
            break;
          case test::FdAction::kind_t::write_quota:
            state.wquota += action.quota;
            break;
          }
        }
      }

      // are {read,write} ready?
      bool read_ready =
        state.want_read && 0 < state.rquota && !state.script.input.empty();
      bool write_ready =
        state.want_write && (state.winprogress || 0 < state.wquota);

      // edge detection
      bool fire_read = read_ready && !state.prev_read_ready;
      bool fire_write = write_ready && !state.prev_write_ready;

      bool fire_err = state.err && !state.delivered_err;
      bool fire_hup = state.hup && !state.delivered_hup;

      // push event
      if (fire_read || fire_write || fire_err || fire_hup)
      {
        events[i++] = {
          .readable = fire_read,
          .writeable = fire_write,

          .signal = false,
          .close = fire_hup,
          .error = fire_err,

          .user_data = state.user_data
        };

        // reset flags
        if (fire_write)
          state.winprogress = false;
        if (fire_err)
          state.delivered_err = true;
        if (fire_hup)
          state.delivered_hup = true;
      }

      // update previous readiness
      state.prev_read_ready = read_ready;
      state.prev_write_ready = write_ready;

      // drop socket if done and continue
      if (state.delivered_err || state.delivered_hup)
      {
        it = _sockets.erase(it);
      }
      else
      {
        ++it;
      }
    }

    return static_cast<int>(i);
  }

  // events
  static bool ev_signal(const event_t &ev) noexcept { return ev.signal; }

  static bool ev_close(const event_t &ev) noexcept { return ev.close; }

  static bool ev_error(const event_t &ev) noexcept { return ev.error; }

  static bool ev_readable(const event_t &ev) noexcept { return ev.readable; }

  static bool ev_writeable(const event_t &ev) noexcept { return ev.writeable; }

  static void *get_user_data(const event_t &ev) noexcept
  {
    return ev.user_data;
  }

  // event subscriptions
  static void
  subscribe(void *ptr, fd_t fd, bool want_read, bool want_write) noexcept
  {
    assert(want_read || want_write);
    if (!_sockets.contains(fd))
      return;

    auto &s = _sockets[fd];

    s.user_data = ptr;
    s.want_read = want_read;
    s.want_write = want_write;

    s.prev_read_ready = false;
    s.prev_write_ready = false;
  }

  static void clear(fd_t fd) noexcept
  {
    if (!_sockets.contains(fd))
      return;

    auto &s = _sockets[fd];

    s.user_data = nullptr;
    s.want_read = false;
    s.want_write = false;

    s.prev_read_ready = false;
    s.prev_write_ready = false;
  }

  static std::span<const std::byte> _output(fd_t fd)
  {
    if (!_outputs.contains(fd))
      return {};
    return std::span(_outputs[fd]);
  }

private:
  static inline config_t _scripts;
  static inline fd_t _next_fd = 0;
  static inline std::size_t _signals = 0;
  static inline std::unordered_map<fd_t, test::FdState> _sockets = {};
  static inline std::unordered_map<fd_t, std::vector<std::byte>> _outputs = {};
};

static std::deque<test::FdAction>
gen_script(std::initializer_list<std::string_view> inputs)
{
  std::deque<test::FdAction> script = {};

  for (auto input : inputs)
  {
    script.push_back(test::FdAction::GrantRead(input.size()));
    script.push_back(test::FdAction::GrantWrite(input.size()));
  }

  return script;
}

static std::string to_string(std::deque<test::FdAction> script)
{
  std::string str;
  str.append("{");

  bool first = true;
  for (auto &act : script)
  {
    if (!first)
      str.append(", ");

    switch (act.kind)
    {
    case test::FdAction::kind_t::read_quota:
      str.append("R(");
      break;
    case test::FdAction::kind_t::write_quota:
      str.append("W(");
      break;
    }

    str.append(std::to_string(act.quota));
    str.append(")");

    first = false;
  }
  str.append("}");

  return str;
}

} // namespace manet::net
