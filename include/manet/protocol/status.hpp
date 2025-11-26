#pragma once

#include <cstdint>
#include <string_view>

namespace manet::protocol
{

/** protocol layer status */
enum class Status : uint8_t
{
  ok,
  close,
  error
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
  }
}

} // namespace manet::protocol
