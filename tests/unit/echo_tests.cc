// #include <bit>
#include <cstdint>
#include <deque>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <variant>

#include "manet/reactor/io.hpp"
#include "manet/transport/plain.hpp"

#include "mock/reactor.hpp"

namespace manet::protocol
{

struct EchoTest
{
  using ctx_t = std::monostate;
  using args_t = std::monostate;

  static inline ctx_t init(std::string_view, uint16_t, args_t) noexcept
  {
    return {};
  }

  static inline Status on_connect(ctx_t &ctx, reactor::TxSink out) noexcept
  {
    CHECK(2 < out.wbuf().size());
    out.wbuf().data()[0] = std::byte{'A'};
    out.wbuf().data()[1] = std::byte{'B'};
    out.wrote(2);
    return Status::ok;
  }

  static inline Status on_data(ctx_t &ctx, reactor::IO io) noexcept
  {
    auto r = io.rbuf().size();
    auto w = io.wbuf().size();

    auto L = std::min(r, w);

    for (int i = 0; i < L; i++)
    {
      io.wbuf().data()[i] = io.rbuf()[i];
    }

    io.read(L);
    io.wrote(L);

    return Status::ok;
  }
};

} // namespace manet::protocol

#define ECHO_TEST                                                              \
  manet::reactor::test::reactor_test1<                                         \
    manet::transport::Plain, manet::protocol::EchoTest>

#define R manet::net::test::FdAction::GrantRead
#define W manet::net::test::FdAction::GrantWrite

TEST_CASE("asynchronous connect<Plain,EchoTest>()")
{
  ECHO_TEST(true, "", "", {});
  ECHO_TEST(true, "", "A", {W(1)});
  ECHO_TEST(true, "ab", "AB", {W(2)});

  ECHO_TEST(true, "ab", "AB", {W(3)});
  ECHO_TEST(true, "ab", "ABa", {W(2), R(1), W(1)});
  ECHO_TEST(true, "ab", "ABa", {W(2), R(1), W(2)});
  ECHO_TEST(true, "ab", "ABa", {W(2), R(2), W(1)});

  ECHO_TEST(true, "ab", "ABab", {W(2), R(2), W(2)});
  ECHO_TEST(true, "abcd", "ABabcd", {W(1), W(2), R(2), R(2), W(2), W(1)});
}

TEST_CASE("synchronous  connect<Plain,EchoTest>()")
{
  ECHO_TEST(false, "", "", {});
  ECHO_TEST(false, "", "A", {W(1)});
  ECHO_TEST(false, "ab", "AB", {W(2)});

  ECHO_TEST(false, "ab", "AB", {W(3)});
  ECHO_TEST(false, "ab", "ABa", {W(2), R(1), W(1)});
  ECHO_TEST(false, "ab", "ABa", {W(2), R(1), W(2)});
  ECHO_TEST(false, "ab", "ABa", {W(2), R(2), W(1)});
  ECHO_TEST(false, "ab", "ABab", {W(2), R(2), W(2)});
}

static void
test_all_interleavings_after_connect(bool async, std::string const &input)
{
  const std::size_t N = input.size(); // 6

  for (std::size_t l = 0; l <= N; ++l)
  {
    // We fix connect funding so "AB" is always flushable.
    // Remaining steps interleave echo read/write credits:
    //   l reads (R1) and l writes (W1) in any order.
    const std::size_t steps = 2 * l;
    const std::uint64_t all_masks = (steps == 64) ? ~0ull : (1ull << steps);

    for (std::uint64_t mask = 0; mask < all_masks; ++mask)
    {
      if (std::popcount(mask) != l)
        continue; // exactly l reads

      std::deque<manet::net::test::FdAction> acts;
      acts.push_back(W(2)); // fund "AB" first

      std::size_t rleft = l, wleft = l;
      bool ok = true;

      for (std::size_t pos = 0; pos < steps; ++pos)
      {
        const bool is_read = (mask >> pos) & 1ull;
        if (is_read)
        {
          if (rleft == 0)
          {
            ok = false;
            break;
          }
          acts.push_back(R(1));
          --rleft;
        }
        else
        {
          if (wleft == 0)
          {
            ok = false;
            break;
          }
          acts.push_back(W(1));
          --wleft;
        }
      }
      if (!ok || rleft || wleft)
        continue;

      std::string expected = "AB";
      expected += input.substr(0, l);

      ECHO_TEST(async, input, expected, acts);
    }
  }
}

static void
test_all_interleavings_everything(bool async, std::string const &input)
{
  const std::size_t N = input.size(); // 6

  for (std::size_t l = 0; l <= N; ++l)
  {
    // Full shuffle of:
    //   R1 x l
    //   W1 x (2 + l)
    const std::size_t steps = (2 + l) + l; // 2 + 2l
    const std::uint64_t all_masks = (steps == 64) ? ~0ull : (1ull << steps);

    for (std::uint64_t mask = 0; mask < all_masks; ++mask)
    {
      if (std::popcount(mask) != l)
        continue; // positions of reads

      std::deque<manet::net::test::FdAction> acts;
      std::size_t rleft = l, wleft = 2 + l;
      bool ok = true;

      for (std::size_t pos = 0; pos < steps; ++pos)
      {
        const bool is_read = (mask >> pos) & 1ull;
        if (is_read)
        {
          if (rleft == 0)
          {
            ok = false;
            break;
          }
          acts.push_back(R(1));
          --rleft;
        }
        else
        {
          if (wleft == 0)
          {
            ok = false;
            break;
          }
          acts.push_back(W(1));
          --wleft;
        }
      }
      if (!ok || rleft || wleft)
        continue;

      std::string expected = "AB";
      expected += input.substr(0, l);

      ECHO_TEST(async, input, expected, acts);
    }
  }
}

TEST_CASE("interleavings")
{
  // same as _everything
  test_all_interleavings_after_connect(false, "");

  std::string input = "12345";

  for (int i = 1; i < input.size(); i++)
  {
    test_all_interleavings_after_connect(false, input.substr(0, i));
    test_all_interleavings_everything(false, input.substr(0, i));
  }
}
