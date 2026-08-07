# coro — C++23 coroutine vocabulary types

`Task`, `WhenAll`, `WhenAny`, `Cancellation`, `Awaitable`, `UniqueCoroHandle`: the
dependency-free coroutine building blocks used by `src/net`'s reactor and the multiplexer
daemon.

`coro` has **no first-party dependencies** — its shipped headers include nothing but the
standard library. Keep it that way; `src/net` is the layer that is allowed to know about
sockets, and neither may depend on anything above it in the tree.

## Provenance — this copy is canonical

This module and `src/net` originated in the [Endo](https://github.com/contour-terminal/endo)
project (Apache-2.0, same author), which in turn drew on the async layer in
[fastcached](https://github.com/LASTRADA-Software/fastcached). **Contour's copy is now the
source of truth for all three.** It has since gained `UniqueCoroHandle`, the `net::EventLoop`
/ `net::EventSource` split (replacing Endo's TUI-coupled `TuiRuntime`), TLS, Unix-domain
sockets, fd passing, and a much larger test suite.

Changes therefore flow **outward** — fix here first, then propagate. Do not re-sync *from*
Endo or fastcached: their copies are older and, in Endo's case, a strict subset.

> An earlier revision of this file prescribed a `sed`-based re-sync *from* Endo. That recipe
> is destructive as of the divergence above — running it would silently revert
> `UniqueCoroHandle`, `EventLoop`, TLS and Unix-socket support. It has been removed.

Consuming projects are expected to pull this code in at CMake configure time rather than
vendor a copy into their own `src/`, so that a downstream edit cannot quietly fork it again.

## Conventions

- Coroutine parameters are **pointers, never references** — a reference parameter dangles once
  the coroutine suspends (`cppcoreguidelines-avoid-reference-coroutine-parameters`, enforced by
  the root `.clang-tidy`). Non-coroutine functions keep references: `listen(EventLoop&, …)` but
  `connect(EventLoop*, …)`.
- Coroutine awaiter and promise hooks (`await_ready`, `await_suspend`, `await_resume`,
  `initial_suspend`, `final_suspend`, `return_value`, …) are named by the language, so each
  carries a `// NOLINTNEXTLINE(readability-identifier-naming)` at its declaration. They must
  also stay non-static instance methods: a static `initial_suspend`/`final_suspend` makes the
  compiler-generated `promise.hook()` call trip `readability-static-accessed-through-instance`.
  `readability-convert-member-functions-to-static`, which would otherwise flag the stateless
  ones, is disabled tree-wide in the root `./.clang-tidy` — there is no directory-local config.
- Cancellation is the standard `<stop_token>` facility, re-exported as `coro::StopToken` /
  `StopSource` / `StopCallback` so call sites stay stable. A cancelled frame unwinds by
  throwing `coro::OperationCancelled`.
