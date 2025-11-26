#pragma once

#include "manet/utils/hexdump.hpp"

namespace manet::reactor
{

template <std::size_t CAP> class Buffer
{
public:
  Buffer() {}

  std::span<const std::byte> rbuf()
  {
    return std::span(_buf).subspan(_rpos, _wpos - _rpos);
  }

  std::span<std::byte> wbuf()
  {
    return std::span(_buf).subspan(_wpos, CAP - _wpos);
  }

  void clear() { _rpos = _wpos = 0; }

  void inc_wpos(std::size_t len) { _wpos += len; }
  void inc_rpos(std::size_t len)
  {
    _rpos += len;
    if (_rpos == _wpos)
    {
      _rpos = _wpos = 0;
    }
  }

  bool full() { return CAP == _wpos; }

  std::string hexdump()
  {
    return utils::hexdump(
      std::span<std::byte>(_buf).subspan(_rpos, _wpos - _rpos)
    );
  }

  // make iterable
  using iterator = std::byte *;

  iterator begin() { return static_cast<iterator>(_buf.data() + _rpos); }
  iterator end() { return static_cast<iterator>(_buf.data() + _wpos); }

  iterator begin() const { return _buf.data() + _rpos; }
  iterator end() const { return _buf.data() + _wpos; }

private:
  std::array<std::byte, CAP> _buf{};

  std::size_t _rpos = 0;
  std::size_t _wpos = 0;
};

} // namespace manet::reactor
