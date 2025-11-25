#include <doctest/doctest.h>

#include <manet/net/posix.hpp>

TEST_CASE("test imports") { CHECK(manet::net::Posix::name == "POSIX"); }
