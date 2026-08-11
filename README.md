# exchange

A multi-threaded TCP exchange server in C++20.

Clients connect over TCP and submit orders through a length-prefixed binary
protocol. A thread pool decodes and validates them, a single engine thread owns
the order book and performs matching, fills are published back to subscribed
sessions, and state transitions are durably logged so the process can crash and
recover to a consistent state.

> **Status:** under construction. This README covers building and testing; the
> architecture write-up lands when the system is complete.

## Requirements

Linux only — the server is built against `epoll`, POSIX socket options and
`fsync` semantics directly, rather than behind a portability layer. Under
Windows, build inside WSL.

- A C++20 compiler (developed against GCC 13.3)
- CMake 3.25 or newer
- Network access on first configure, to fetch GoogleTest

## Build

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

`RelWithDebInfo` is the default build type. The project compiles with
`-Wall -Wextra -Werror` and a broader warning set on top; warnings are errors
from the first commit.

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Sanitizers

Each sanitizer needs its own build directory, since instrumentation is a
compile-time property:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DEXCHANGE_SANITIZER=thread
cmake --build build-tsan -j"$(nproc)"
ctest --test-dir build-tsan --output-on-failure
```

Valid values are `none` (default), `address`, `thread` and `undefined`. The
`thread` configuration runs test binaries under `setarch -R`, because
ThreadSanitizer's shadow memory conflicts with the ASLR entropy that Linux 6.x
enables by default.

## Build options

| Option | Default | Effect |
|---|---|---|
| `EXCHANGE_BUILD_TESTS` | `ON` | Build the GoogleTest suite |
| `EXCHANGE_BUILD_BENCH` | `OFF` | Build the replay benchmark driver |
| `EXCHANGE_SANITIZER` | `none` | `none` \| `address` \| `thread` \| `undefined` |

## Layout

| Directory | Contents |
|---|---|
| `core/` | Order hierarchy, `OrderBook<PriceCompare>`, matching strategies, book iterators |
| `net/` | epoll reactor, sessions, binary codec, publisher |
| `concurrent/` | `ThreadSafeQueue<T>`, thread pool |
| `store/` | Write-ahead log, SQLite store, recovery |
| `analytics/` | VWAP, depth, imbalance, L2 snapshot |
| `tests/` | GoogleTest suites and fault-injection helpers |
| `bench/` | Replay driver and latency histogram |
