# core-cpp

[![Build](https://github.com/contour-terminal/core-cpp/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/contour-terminal/core-cpp/actions/workflows/build.yml)
[![Docs](https://github.com/contour-terminal/core-cpp/actions/workflows/docs.yml/badge.svg?branch=master)](https://contour-terminal.github.io/core-cpp/)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)

core-cpp is the shared C++23 foundation of the Contour Terminal projects: base utilities,
logging, command-line parsing, an operating-system layer, coroutines, an event loop with
sockets and TLS, and a terminal UI. It replaces the near-verbatim copies of this code that
contour, endo, fastcached and tuidu each carried, and it merges contour's and fastcached's two
coroutine and networking designs into one. Everything is in namespace `core`, one namespace per
directory. A defined subset (base, log, cli, async and testing, and parts of platform and net) is
held to building and passing its tests under single-threaded WebAssembly.

**Status: 0.1.0 is in development.** The build framework and every module exist:
`core::base`, `core::log`, `core::cli`, `core::platform`, `core::async`, `core::net`,
`core::tui` and `core::testing`. The remaining tasks of the
[implementation plan](docs/superpowers/plans/2026-09-18-core-cpp.md) merge fastcached's executors
and networking layer into `core::async` and `core::net`, and move the TUI runtime onto the event
loop; the table below says what each module has today. Nothing is tagged yet.

## Modules

| Module | Namespace | Target(s) | Depends on | Contents | Status |
|---|---|---|---|---|---|
| base | `core` | `core::base` | Threads; Tracy (optional) | assertions, environment, escaping, hashing, flags, time, `Base64`, `Generator`, profiling macros, range helpers | **available** |
| log | `core::log` | `core::log` | base | log store and sinks | **available** |
| cli | `core::cli` | `core::cli` | base, log | command-line parser, application scaffold | **available** |
| platform | `core::platform` | `core::platform` | base, log | clocks, wakeup, signals, pipes, file system, environment, paths | **available** |
| async | `core::async` | `core::async` (header-only) | the standard library | `StopToken`, `Task`, cancellation, `whenAll`/`whenAny`, executors, `AsyncQueue` | `StopToken`, `Task`, cancellation and the combinators **available** (A5); executors and `AsyncQueue` planned (B1) |
| net | `core::net` | `core::net_types` (header-only), `core::net`, `core::net_tls` (with `CORE_CPP_WITH_TLS`) | async, platform; OpenSSL for TLS | event loop and backends (epoll, kqueue, poll; IOCP and host-driven planned), TCP and AF_UNIX sockets, descriptor passing, buffered reading, a write queue, timeouts, TLS, HTTP server; dialling planned | contour's event loop, sockets, TLS and HTTP server **available** (A6), native only but for `core::net_types`; the merge with fastcached's planned (B2-B11) |
| tui | `core::tui` | `core::tui_output`, `core::tui` (with `CORE_CPP_WITH_TUI`) | base; the full TUI also platform, async, net, libunicode, and stb with `CORE_CPP_WITH_IMAGES` | terminal output and input, screen and widgets, completion, Markdown and syntax highlighting, sixel images, a coroutine runtime | endo's terminal UI **available** (A7), native only; its runtime is composed on `core::net::EventLoop` (B12) |
| testing | `core::testing` | `core::testing`, `core::testing_dialogs`, `core::testing_main` | base; log and Catch2 for `testing_main` | Windows dialog suppression, a fake environment, scoped temporary directory, working directory and environment variable, a Catch2 `main()` with the `LOG` filter and a normalised exit code | **available** |

The layering is enforced: a module links only the modules its row in
[`cmake/CoreCppModules.cmake`](cmake/CoreCppModules.cmake) lists.

## Using it with CPM

```cmake
CPMAddPackage(
    NAME core-cpp
    GITHUB_REPOSITORY contour-terminal/core-cpp
    GIT_TAG v0.5.0
    SYSTEM YES              # core-cpp headers never trip your -Werror
    EXCLUDE_FROM_ALL YES    # build only what you link
    OPTIONS "CORE_CPP_WITH_TUI ON" "CORE_CPP_WITH_TLS OFF")
target_link_libraries(myapp PRIVATE core::async core::net core::tui)
# local development against a checkout: -DCPM_core-cpp_SOURCE=/path/to/core-cpp
```

As a subproject core-cpp changes nothing of its parent's: no compiler launcher, no C++
standard, no directory-wide flag, and every option is `CORE_CPP_`-prefixed. Pin a tag, never a
branch. The options are listed in
[the documentation](https://contour-terminal.github.io/core-cpp/getting-started/options/).

## Vendoring

A project that must build without fetching anything can carry a verbatim copy instead. contour
consumes core-cpp this way:

```sh
# Copy a tag into your tree, then commit the result as one change. The script is a core-cpp
# checkout's, not the copy's: sync reads a repository, and the repository it reads by default is
# the one the script itself is in.
cmake -DMODE=sync -DREF=v0.5.0 -DDEST=vendor/core-cpp \
      -P /path/to/core-cpp/cmake/CoreCppVendor.cmake

# Verify it. Needs no git, so register it as a test of your own suite.
cmake -DMODE=check -DDEST=vendor/core-cpp -P vendor/core-cpp/cmake/CoreCppVendor.cmake
```

`sync` copies a tag's files out of git's own blobs and writes a manifest of their hashes; `check`
refuses any local change, missing file or extra file. The contract, the file set and your
obligations as a consumer are in [`docs/vendoring.md`](docs/vendoring.md).

## Building

Every build uses a preset and builds into `out/build/<preset>`:

```sh
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug
cmake --workflow --preset ci-gcc-release      # configure, build and test in one step
```

| Host | Presets |
|---|---|
| Linux | `clang-debug`, `clang-release`, `gcc-debug`, `gcc-release`, `clang-asan-ubsan`, `clang-tsan`, `clang-tidy`, `clang-coverage`, `clang-tracy` |
| macOS | `appleclang-debug`, `appleclang-release`, and the `clang-*` presets with Homebrew LLVM |
| Windows (from a Visual Studio developer shell) | `cl-debug`, `cl-release`, `clangcl-debug`, `clangcl-release`, `cl-release-tls`; `cl-release-arm64` from an arm64 one |
| Any, with emsdk (`EMSDK` set) | `emscripten`: single-threaded WebAssembly, tests run under node |

The build goes through fastcache-cc when a fastcached daemon answers, otherwise ccache; see
[`cmake/portable/README.md`](cmake/portable/README.md).

## Requirements

- CMake 3.25 or newer, and Ninja.
- A C++23 compiler: clang 22, GCC 14, AppleClang from Xcode 16, or Visual Studio 2022 or newer
  (`cl` or `clang-cl` 22).
- Python 3, for the formatting and tool-version scripts.
- For WebAssembly: emsdk 3.1.56 or newer, and node.
- Dependencies are resolved from your project, then `find_package`, then fetched with CPM:
  Catch2 3.8.0 (tests), libunicode (TUI), stb (TUI images), Tracy (profiling), and OpenSSL from
  the system (TLS).

## Used by

| Project | How |
|---|---|
| [contour](https://github.com/contour-terminal/contour) | vendored |
| [endo](https://github.com/contour-terminal/endo) | CPM |
| [fastcached](https://github.com/LASTRADA-Software/fastcached) | CPM |
| [tuidu](https://github.com/contour-terminal/tuidu) | CPM |
| [Lightweight](https://github.com/LASTRADA-Software/Lightweight) `dbtool` | CPM |
| [morph](https://github.com/LASTRADA-Software/morph) | CPM, including its WebAssembly build |

Each migrates onto core-cpp after `v0.1.0` is tagged.

## Documentation

<https://contour-terminal.github.io/core-cpp/>, with the API reference at
<https://contour-terminal.github.io/core-cpp/api/>. Contributing: [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

Apache License 2.0; see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE), which names every project
code was imported from and the commit it was imported at.
