# manet

![GitHub Actions CI](https://github.com/b4er/manet/actions/workflows/ci.yaml/badge.svg?branch=main)

manet is a network I/O library providing a single-threaded, edge-triggered
reactor for client-side connection handling. It targets [F-Stack][F-Stack] as a
backend for low-latency, kernel-bypassed networking, and also provides a Linux
`epoll` backend (useful for development and testing).


## Quickstart

The reactor's connections are templated and kept generic:

```cpp
using namespace manet;

using Net = net::Epoll;
// or: using Net = net::FStack;

using Transport = transport::Tls;
// or: using Transport = transport::Plain;

using Connection_1 = Connection<Net, Transport, Protocol>;
```

A protocol defines asynchronous handlers for connecting, handling data and/or
shutting down gracefully:

```cpp
struct Protocol
{
  using config_t = /* user-defined */;

  struct Session
  {
    Session(std::string_view host, uint16_t port, config_t& config) noexcept;

    /** called once as soon as Transport connected successfully */
    protocol::Status on_connect(reactor::IO output) noexcept; // optional

    /** called repeatedly as long as there is data to be consumed */
    protocol::Status on_data(reactor::IO output) noexcept;

    /** called until notified to close connection */
    protocol::Status on_shutdown(reactor::IO output) noexcept; // optional

    /** called whenever the connection entered an error or closing state */
    protocol::Status teardown() noexcept; // optional
  };
};
```

A reactor declares a static set of connections and provides `.run()` to
initialize the event listeners and start all connections:

```cpp
// std::optional<std::string> fstack_config_file = "config.ini";

Reactor<Net, Connection_1, Connection_2, ...> reactor;

reactor.run(fstack_config_file, std::make_tuple(
  connection_config_1, connection_config_2, ...
));
```

To interrupt the reactor and shut it down call `Net::signal()`. For more
information please refer to the [examples](./examples/main.cc).


## Development

Either from within a `nix develop` shell (easiest), or using CMake using C++23.

To run the examples and tests you will need (provided by the nix shell)

- tooling: cmake and lcov
- C++ dependencies: doctest, libfmt, libpugixml, [sbeppc][sbeppc], [SPSCQueue][SPSCQueue]
- Python dependency: `websockets`  (for integration tests)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

ctest --test-dir build
```

(refer to the [CI-script](./.github/workflows/ci.yaml) for information on how to
setup the C++ dependencies)


<!-- references: -->

[F-Stack]: https://github.com/F-Stack/f-stack

[sbeppc]: https://oleksandrkvl.github.io/

[SPSCQueue]: https://github.com/rigtorp/SPSCQueue
