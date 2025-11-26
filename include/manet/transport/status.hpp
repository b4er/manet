#pragma once

#include <cstdint>
#include <string_view>

namespace manet::transport
{

/** transport layer status */
enum class Status : uint8_t
{
  ok,        // made progress or would block
  close,     // peer closed / EOF
  error,     // IO error
  want_read, // arm read edge interest
  want_write // arm write edge interest
};

[[nodiscard]] constexpr std::string_view to_string(Status status)
{
  switch (status)
  {
  case Status::ok:
    return "ok";
  case Status::close:
    return "close";
  case Status::error:
    return "error";
  case Status::want_read:
    return "want_read";
  case Status::want_write:
    return "want_write";
  }
  return "~";
}

} // namespace manet::transport
