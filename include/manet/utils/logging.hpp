#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <unistd.h>

namespace manet::utils
{

inline constexpr bool logging_enabled =
#ifdef DEBUG
  true;
#else
  false;
#endif

enum class LogLevel : std::uint8_t
{
  trace,
  info,
  warn,
  error,
};

inline std::atomic<LogLevel> _g_level{LogLevel::warn};

template <typename... Args>
inline void log(LogLevel level, std::string_view fmt, Args &&...args) noexcept
{
  // compile out in non-DEBUG builds
  if constexpr (!logging_enabled)
  {
    (void)level;
    (void)fmt;
    (void)std::initializer_list<int>{((void)args, 0)...};
  }
  else
  {
    if (level < _g_level.load(std::memory_order_relaxed))
    {
      return;
    }

    static constexpr std::array<std::string_view, 4> prefixes = {
      "[Debug]   ",
      "[Info]    ",
      "[Warning] ",
      "[Error]   ",
    };

    std::string formatted;
    try
    {
      formatted = std::vformat(fmt, std::make_format_args(args...));
    }
    catch (...)
    {
      formatted = std::string(fmt);
    }

    const auto idx = static_cast<std::uint8_t>(level);
    const std::string_view pfx = prefixes[idx];

    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    ::write(STDERR_FILENO, pfx.data(), pfx.size());
    ::write(STDERR_FILENO, formatted.data(), formatted.size());
    ::write(STDERR_FILENO, "\n", 1);
  }
}

inline void set_level(LogLevel level) noexcept
{
  if constexpr (logging_enabled)
  {
    _g_level.store(level, std::memory_order_relaxed);
  }
  else
  {
    (void)level;
  }
}

// optional sugar
template <typename... Args>
inline void trace(std::string_view fmt, Args &&...args) noexcept
{
  log(LogLevel::trace, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void info(std::string_view fmt, Args &&...args) noexcept
{
  log(LogLevel::info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void warn(std::string_view fmt, Args &&...args) noexcept
{
  log(LogLevel::warn, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void error(std::string_view fmt, Args &&...args) noexcept
{
  log(LogLevel::error, fmt, std::forward<Args>(args)...);
}

} // namespace manet::utils
