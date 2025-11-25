#pragma once
#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <string_view>
#include <unistd.h>

enum class LogLevel : uint8_t
{
  trace,
  info,
  warn,
  error
};

struct NoLogging
{
  static constexpr bool enabled = false;

  template <typename... Args>
  static inline void log(LogLevel, std::string_view, Args &&...) noexcept
  {
  }
  static inline void set_level(LogLevel) noexcept {}
};

struct StderrLogging
{
  static inline std::atomic<LogLevel> _level{LogLevel::warn};
  static constexpr bool enabled = true;

  template <typename... Args>
  static inline void
  log(LogLevel level, std::string_view fmt, Args &&...args) noexcept
  {
    if (level < _level.load(std::memory_order_relaxed))
    {
      return;
    }

    static constexpr std::array<std::string_view, 4> prefixes = {
      "[Debug]   ", "[Info]    ", "[Warning] ", "[Error]   "
    };

    std::string formatted;
    try
    {
      formatted = std::vformat(fmt, std::make_format_args(args...));
    }
    catch (...)
    {
      // If format fails, fallback to raw format string
      formatted = std::string(fmt);
    }

    const std::string_view pfx = prefixes[static_cast<uint8_t>(level)];

    static std::mutex mutex;
    {
      std::lock_guard<std::mutex> lock(mutex);
      ::write(STDERR_FILENO, pfx.data(), pfx.size());
      ::write(STDERR_FILENO, formatted.data(), formatted.size());
      ::write(STDERR_FILENO, "\n", 1);
    }
  }

  static inline void set_level(LogLevel level) noexcept
  {
    _level.store(level, std::memory_order_relaxed);
  }
};

#ifdef DEBUG
using Log = StderrLogging;
#else
using Log = NoLogging;
#endif
