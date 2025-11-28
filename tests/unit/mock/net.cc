#include "net.hpp"

std::deque<FdAction> gen_script(std::initializer_list<std::string_view> inputs)
{
  std::deque<FdAction> script = {};

  for (auto input : inputs)
  {
    script.push_back(FdAction::GrantRead(input.size()));
    script.push_back(FdAction::GrantWrite(input.size()));
  }

  return script;
}
