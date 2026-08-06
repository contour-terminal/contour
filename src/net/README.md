# net — coroutine-native networking and event loop

The reactor, socket layer and TLS transport that `src/vthost` and `src/contour/remote` are
built on. Built on `src/coro`; depends on nothing above it in the tree.

See [`../coro/README.md`](../coro/README.md) for provenance — **this copy is canonical** for
Contour, Endo and fastcached alike, and changes flow outward from here.

## Layout

| Path | Contents |
|---|---|
| `EventLoop.*` | Scheduling, timers, `post()`, spawned-flow reaping. Owns the ready queue. |
| `EventSource.hpp` | The DI seam over "block until something happens". `FdRegistry`, `FdToken`, `FdInterest`, `WaitOutcome`. |
| `PollEventSource.*` | The portable `EventSource`: `poll(2)` on POSIX, `WaitForMultipleObjects` on Windows. |
| `EpollEventSource.*`, `KqueueEventSource.*` | Native `EventSource`s for Linux and macOS/BSD. Behaviourally identical to poll; a wait is O(ready) rather than O(registered). |
| `DefaultEventSource.*` | `makeDefaultEventSource()` — picks the best backend, falling back to poll. Use this rather than naming a backend. |
| `ISocket.hpp`, `IListener.hpp`, `IoResult.hpp` | Transport interfaces and the `std::expected` error vocabulary. |
| `Sockets.hpp` | `listen`/`connect`/`listenUnix`/`connectUnix`/`adoptFd` free functions. |
| `Tls.*` | TLS decorator behind `ITlsContext`; OpenSSL stays private to the `.cpp`. |
| `AsyncBufferedReader.*`, `WriteQueue.*`, `SplitSocket.hpp`, `WithTimeout.hpp` | Composition helpers over `ISocket`. |
| `platform/`, `posix/`, `windows/` | Per-OS implementation details. These directories do **not** introduce sub-namespaces. |
| `testing/` | Test doubles: `ScriptedEventSource`, `ManualClock` (in `platform/Clock.hpp`), `makeSocketPair`, `TempDir`. |

## Invariants worth not breaking

- **`EventSource` is the extension point.** Adding a native reactor means adding an
  implementation, not changing the interface — `EventLoop` already owns timers and the ready
  queue, so a new source implements only the blocking wait plus fd registration. Every backend
  must be behaviourally interchangeable; `EventSourceParity_test.cpp` runs the same scenarios
  against each one to keep them so. IOCP is the exception that proves the rule — it is
  completion-based rather than readiness-based, so it does not fit this interface at all (see
  `DefaultEventSource.hpp`).
- **A failed fd registration fails the awaitable; it never parks.** `EventLoop::registerFdWaiter`
  returns `FdToken::invalid()` on failure and `WaitFdAwaiter::await_resume()` throws
  `FdRegistrationFailed`. Suspending on an interest the kernel never registered is unresumable —
  the connection would hang forever holding untransferred bytes. This is a real bug that has been
  hit in a sibling copy of this code; preserve the invariant in any new `EventSource`.
- **Readiness is level-triggered.** `PosixSocket` and the accept paths assume a still-ready fd is
  reported again on the next wait. An edge-triggered source must either emulate that or come with
  a socket layer that drains to `EAGAIN`.
- **I/O errors are `std::expected`, not exceptions.** Exceptions are reserved for control flow:
  `coro::OperationCancelled` and `net::FdRegistrationFailed`.
- **No OpenSSL type crosses a `net` header.** `Tls.cpp` keeps it behind `ITlsContext`, which is
  why `OpenSSL::SSL` is linked `PRIVATE`.
