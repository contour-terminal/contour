# Wake-Resize After Reconnect Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A daemon-hosted pane is never left at the profile size after a reconnect, and opening the lid no longer drops a healthy daemon connection.

**Architecture:** core-cpp gains `core::platform` system sleep/wake notifications (IOKit, logind via sd-bus, Win32) behind an injectable interface. Contour's liveness check treats a late tick or an OS wake as "we were frozen" and probes with a short deadline instead of declaring the connection lost. The reconnect hello states the window's real client area, and the per-pane sizes are re-sent once after the reconnect's layout is reconciled.

**Tech Stack:** C++23, CMake presets, Catch2, Qt6 (GUI tests), IOKit, libsystemd (sd-bus), Win32 power API.

**Spec:** `docs/superpowers/specs/2026-11-03-wake-resize-after-reconnect-design.md`

## Global Constraints

- Contour work: worktree `/Users/christianparpart/projects/contour-wake-resize`, branch `fix/wake-resize-after-reconnect`. core-cpp work: create worktree `/Users/christianparpart/projects/core-cpp-system-power` from `origin/master`, branch `feature/system-power`.
- Both repos follow `AGENT.md` (Contour) / core-cpp's own `AGENT.md`/`CLAUDE.md`: C++23, `enum class : uint8_t` with the off/default case at zero, no `bool` in API surface, `std::expected` for fallible setup, Doxygen on public API, `.hpp`, `#pragma once`, no `NOLINT`, `-Werror` clean, clang-format C++ only.
- **Never edit `vendor/core-cpp` by hand.** Re-vendor with the procedure in `vendor/README.md`.
- No AI attribution trailers. Every commit ends with `Signed-off-by: Christian Parpart <christian@parpart.family>`.
- Liveness defaults: `awakeGap = 5 s`, `wakeDeadline = 5 s`; existing `idleBeforeProbe = 5 s`, `deadAfter = 15 s` unchanged.
- CMake option: `option(CORE_CPP_WITH_SYSTEMD "Notify of system sleep/wake on Linux through logind (libsystemd)" ON)`, Linux only, `pkg-config libsystemd`, ON-but-missing is a configure error.
- No sleep inhibition and no blocking of `IOAllowPowerChange` beyond the handlers' return.
- Split panes may receive one extra resize after a reconnect (accepted; per-pane hello sizes are a follow-up).
- Build/test Contour: `cmake --build --preset clang-asan` (on this Mac: the preset the tree is configured with, e.g. `appleclang-debug`), `ctest --preset=clang-asan`.

## Review Focus

1. **Sleep announced, then cancelled** (macOS `kIOMessageSystemWillNotSleep`, logind `PrepareForSleep(false)` without a sleep): liveness must not stay disabled. Expect: on-time ticks for `awakeGap` after `noteSleep` resume normal liveness. Pinned in Task 6.
2. **Monitor or subscription destroyed while a handler runs** on the monitor's thread (quit during wake): no use-after-free, destructor waits. Pinned in Task 1.
3. **A wake while no connection is up** (Reconnecting): the event is dropped, nothing dereferences a null client. Pinned in Task 9.
4. **A remote daemon that died during the sleep**: lost within `wakeDeadline`, not `deadAfter`. Pinned in Task 7.
5. **A reconnect to a restarted daemon**: the hello still states the real area. Pinned in Task 8.

---

## Part A — core-cpp (`/Users/christianparpart/projects/core-cpp-system-power`)

### Task 1: `SystemPower` API, dispatch registry, fake, unavailable fallback

Mirror the existing `CpuCounters` layout (`CpuCounters.hpp`, `UnavailableCpuCounters.cpp`, `testing/FakeCpuCounterSource.hpp`, the per-platform source chosen once in `src/core/platform/CMakeLists.txt`).

**Files:**
- Create: `src/core/platform/SystemPower.hpp`, `src/core/platform/SystemPower.cpp`, `src/core/platform/UnavailableSystemPower.cpp`, `src/core/platform/testing/FakeSystemPowerMonitor.hpp`, `src/core/platform/SystemPower_test.cpp`
- Modify: `src/core/platform/CMakeLists.txt` (headers, sources, test; add `_coreCppPlatformSystemPowerSource` chosen like `_coreCppPlatformCpuCounterSource`, `UnavailableSystemPower.cpp` for every platform until Tasks 2–4 replace it)

**Interfaces:**
- Produces (namespace `core::platform`):
  ```cpp
  enum class SystemPowerEvent : std::uint8_t { WillSleep = 0, DidWake = 1 };

  /// Move-only; unsubscribes on destruction and waits out a handler running for it.
  class PowerSubscription;  // default-constructible empty token, move ctor/assign, ~PowerSubscription()

  class ISystemPowerMonitor {
    public:
      using Handler = std::function<void(SystemPowerEvent)>;
      virtual ~ISystemPowerMonitor() = default;
      /// Handlers run on a thread the monitor owns; marshal to your own loop.
      [[nodiscard]] virtual PowerSubscription subscribe(Handler handler) = 0;
  };

  namespace detail {
  /// Thread-safe handler table every implementation dispatches through.
  class PowerHandlerRegistry {
    public:
      [[nodiscard]] PowerSubscription subscribe(ISystemPowerMonitor::Handler handler);
      void dispatch(SystemPowerEvent event);   // calls every live handler, in subscription order
      [[nodiscard]] std::size_t size() const;
  };
  }

  [[nodiscard]] std::expected<std::unique_ptr<ISystemPowerMonitor>, PlatformError>
  makeNativeSystemPowerMonitor();
  ```
- `testing::FakeSystemPowerMonitor : ISystemPowerMonitor` with `void emit(SystemPowerEvent)` (dispatches on the calling thread) and `[[nodiscard]] std::size_t subscriberCount() const`.
- `UnavailableSystemPower.cpp` defines `makeNativeSystemPowerMonitor()` returning `std::unexpected(PlatformError::NotImplemented)`.

- [ ] **Step 1: Write failing tests** in `SystemPower_test.cpp` (tag `[platform][power]`):
  - `"a subscriber receives each power event"`: fake, subscribe collecting into a vector, `emit(WillSleep)`, `emit(DidWake)` → vector `== {WillSleep, DidWake}`.
  - `"a destroyed subscription receives nothing"`: subscribe, reset the subscription, `emit(DidWake)` → nothing received, `subscriberCount() == 0`.
  - `"a moved subscription stays subscribed"`: move-construct into another token, emit → received once.
  - `"unsubscribing waits out a running handler"`: registry; an atomic sequence counter; the handler signals `entered`, waits on `release` (a `std::latch`), then records `handlerDone = ++seq`. `dispatch` on a `std::jthread`; main waits for `entered`; a second `std::jthread` destroys the subscription and records `destructorDone = ++seq`; main sleeps 50 ms, `CHECK(destructorDone == 0)` (still blocked), counts down `release`, joins both, `CHECK(handlerDone < destructorDone)`.
  - `"a handler may drop its own subscription"`: the handler resets the token it was subscribed with; `emit` returns (no deadlock); a second `emit` reaches nothing.
  - `"the native monitor constructs or says why not"`: `makeNativeSystemPowerMonitor()` either has a value (subscribe + destroy cleanly) or `error() == PlatformError::NotImplemented`.
- [ ] **Step 2:** Build and run `platform_test "[power]"` → fails to compile (no header).
- [ ] **Step 3:** Implement. Registry: `std::mutex`, entries `{ std::uint64_t id; std::shared_ptr<Handler> handler; }`, a set of `(id, std::thread::id)` currently dispatching, and a `std::condition_variable`. `dispatch`: under the lock snapshot the `shared_ptr`s; then for each, re-check under the lock that it is still subscribed, mark it dispatching on this thread, unlock, invoke, relock, unmark, `notify_all`. `unsubscribe` (the token's destructor): erase the entry, then wait on the cv while that id is dispatching on **another** thread (on the dispatching thread itself — a handler destroying its own token — do not wait). Subscribing from inside a handler is safe because dispatch iterates the snapshot. Add `UnavailableSystemPower.cpp` to `SOURCES_EMSCRIPTEN` too (the module is part of the wasm subset; the header must compile standalone there).
- [ ] **Step 4:** Run `platform_test "[power]"` → PASS. Full `ctest` for core-cpp → PASS.
- [ ] **Step 5:** Commit `platform: add system sleep/wake notifications API`.

### Task 2: macOS backend (IOKit)

**Files:**
- Create: `src/core/platform/darwin/DarwinSystemPower.cpp`
- Modify: `src/core/platform/CMakeLists.txt` (APPLE selects it; link `"-framework IOKit"` `"-framework CoreFoundation"` via `_coreCppPlatformLibs`)

**Interfaces:** Consumes Task 1's `detail::PowerHandlerRegistry`; defines `makeNativeSystemPowerMonitor()` for APPLE.

- [ ] **Step 1:** The Task 1 native test now must take the value branch on macOS: add `#if defined(__APPLE__)` `REQUIRE(monitor.has_value())` to it (the only platform `#if` in the test; the CMake source choice stays the single platform switch for code).
- [ ] **Step 2:** Run → FAIL (`NotImplemented`).
- [ ] **Step 3:** Implement: `IORegisterForSystemPower`, `IONotificationPortSetDispatchQueue` on a private serial `dispatch_queue_t`. `kIOMessageSystemWillSleep` → dispatch `WillSleep`, then `IOAllowPowerChange`. `kIOMessageCanSystemSleep` → `IOAllowPowerChange`, no event. `kIOMessageSystemWillNotSleep` → dispatch `DidWake` (sleep cancelled). `kIOMessageSystemHasPoweredOn` → `DidWake`. Destructor: `IODeregisterForSystemPower`, `IOServiceClose`, `IONotificationPortDestroy`, then drain the queue with `dispatch_sync(queue, ^{})` before releasing it. Registration failure → `PlatformError::NotImplemented` is wrong; add `PlatformError::NotificationRegistrationFailed` at the end of the enum (keeping earlier values) with its `std::string_view` description wherever the enum is described.
- [ ] **Step 4:** Run `platform_test "[power]"` → PASS. Manual: a tiny scratch program (not committed) printing events across `pmset sleepnow` + wake shows `WillSleep`, `DidWake`.
- [ ] **Step 5:** Commit `platform: notify of system sleep/wake on macOS`.

### Task 3: Linux backend (logind over sd-bus) and the CMake option

**Files:**
- Create: `src/core/platform/linux/LinuxSystemPower.cpp`
- Modify: `cmake/CoreCppOptions.cmake` (the option), `src/core/platform/CMakeLists.txt` (LINUX + option selects it, else `UnavailableSystemPower.cpp`; `pkg_check_modules(LIBSYSTEMD libsystemd)` **without** `IMPORTED_TARGET`, `FATAL_ERROR` naming `CORE_CPP_WITH_SYSTEMD` when not found, and link **by value** — `${LIBSYSTEMD_LINK_LIBRARIES}` into `_coreCppPlatformLibs`, `${LIBSYSTEMD_INCLUDE_DIRS}` as a PRIVATE include — exactly as Contour's `src/vtpty/CMakeLists.txt:88-131` does. A `PkgConfig::libsystemd` target would pass `CoreCppInstall.cmake`'s classify but break `tests/consumer-install` on link, since the installed config never re-creates it), `cmake/CoreCppDependencies.cmake` (one sentence in the table's header comment: libsystemd is linked by value and takes no row, with the reason), `.github/workflows/build.yml` (add `libsystemd-dev` to every Linux `apt-get install` incl. the container leg; one Linux leg configures `-DCORE_CPP_WITH_SYSTEMD=OFF`), core-cpp docs listing options.

- [ ] **Step 1:** Extend the native test: on Linux with the option ON (expose `CORE_CPP_WITH_SYSTEMD` to the test as a compile definition `CORE_CPP_HAS_SYSTEMD=1`), `REQUIRE` either a value, or `error() == NotificationRegistrationFailed` when `sd_bus_open_system` fails (CI containers without a system bus).
- [ ] **Step 2:** Run on Linux (CI or a container) → FAIL.
- [ ] **Step 3:** Implement: `sd_bus_open_system`, `sd_bus_match_signal(bus, …, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForSleep", cb, this)`, read the `b` argument: `true` → `WillSleep`, `false` → `DidWake`. A `std::jthread` loops: drain `sd_bus_process` until it returns 0, then `poll` on `sd_bus_get_fd` (events from `sd_bus_get_events`, timeout from `sd_bus_get_timeout`, converted from absolute µs) together with a shutdown `eventfd`. Destructor signals the eventfd, joins, `sd_bus_flush_close_unref`.
- [ ] **Step 4:** Configure with ON and libsystemd missing → configure error naming the option. Build + tests pass with ON (installed) and OFF, including `consumer-install` / `check-install` with ON.
- [ ] **Step 5:** Commit `platform: notify of system sleep/wake on Linux through logind`.

### Task 4: Windows backend

**Files:**
- Create: `src/core/platform/windows/WindowsSystemPower.cpp`
- Modify: `src/core/platform/CMakeLists.txt` (WIN32 selects it; link `powrprof`)

- [ ] **Step 1:** Native test: on Windows `REQUIRE(monitor.has_value())`.
- [ ] **Step 2:** Run (Windows CI) → FAIL.
- [ ] **Step 3:** Implement with `PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &params, &handle)`: `PBT_APMSUSPEND` → `WillSleep`, `PBT_APMRESUMEAUTOMATIC` → `DidWake`, ignore `PBT_APMRESUMESUSPEND`. Destructor: `PowerUnregisterSuspendResumeNotification`, then the registry's wait covers an in-flight callback.
- [ ] **Step 4:** Windows CI green.
- [ ] **Step 5:** Commit `platform: notify of system sleep/wake on Windows`.

### Task 5: Document and release core-cpp

- [ ] **Step 1:** Module docs (`docs/modules/…platform…`): the API, threading contract, backends, the option. Changelog entry.
- [ ] **Step 2:** PR via `/create-pr`; CI green on every leg.
- [ ] **Step 3:** **Ask the user before merging and publishing.** Then merge, `/draft-release` as the next minor (0.13.0), `/publish-release` once all artifacts are attached.

---

## Part B/C — Contour (`/Users/christianparpart/projects/contour-wake-resize`)

Tasks 6–8 need nothing from core-cpp and may start in parallel with Part A.

### Task 6: Liveness survives a frozen process

**Files:**
- Modify: `src/vthost/client/Liveness.hpp`
- Test: `src/vthost/client/Liveness_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct LivenessPolicy {
      // existing three fields unchanged, then:
      /// A tick this much later than due means the process was frozen (sleep, VM pause, SIGSTOP).
      std::chrono::milliseconds awakeGap { 5000 };
      /// After a wake, how long the probe may go unanswered before the connection is lost.
      std::chrono::milliseconds wakeDeadline { 5000 };
  };
  class LivenessTracker {
      // existing members unchanged, plus:
      /// The tick due at @p due ran at @p now: a late tick enters the wake state; an on-time tick
      /// awakeGap after noteSleep() leaves the sleep state (a sleep that was announced and never came).
      void noteTick(core::platform::SteadyTimePoint due, core::platform::SteadyTimePoint now) noexcept;
      /// The OS announced a sleep: decide() returns None until a wake or the fallback above.
      void noteSleep(core::platform::SteadyTimePoint at) noexcept;
      /// The OS announced a wake, or a late tick showed one: silence restarts at @p at, a probe is due
      /// at once, and wakeDeadline applies until the daemon speaks.
      void noteWake(core::platform::SteadyTimePoint at) noexcept;
      /// @return The silence after which decide() says DeclareLost now: wakeDeadline while waking,
      ///         deadAfter otherwise. For the loss message.
      [[nodiscard]] std::chrono::milliseconds silenceLimit() const noexcept;
  };
  ```
  Replace `ProbeState` alone with it plus a private `enum class Phase : uint8_t { Awake = 0, Asleep = 1, Waking = 2 }` and `std::optional<SteadyTimePoint> _asleepSince`. `noteInbound` sets `Phase::Awake` (from `Waking`; it does not end `Asleep`). A late `noteTick` enters `Waking` from **any** phase, `Asleep` included (on macOS the reactor can see [WillSleep] → late tick → [DidWake]). `noteWake` clears an outstanding probe, so the wake probe goes out even if one was out before the sleep; a `noteWake` while already `Waking` restarts the wake deadline. `decide`: `Asleep` → `None`; `Waking` with no probe out → `SendHeartbeat`; silence `>= silenceLimit()` → `DeclareLost`; else as today. `decide` stays `const`.

- [ ] **Step 1: Write failing tests** (tag `[vthost][liveness]`), policy `{ .idleBeforeProbe = 5s, .deadAfter = 15s, .awakeGap = 5s, .wakeDeadline = 5s }`, `t0 = SteadyTimePoint{} + 100s`, `noteInbound(t0)` first:
  - `"a tick late by a sleep probes instead of giving up"`: `noteTick(t0 + 1s, t0 + 1000s)`; `decide(t0 + 1000s) == SendHeartbeat`; after `noteProbeSent(t0 + 1000s)`: `decide(t0 + 1004s) == None`, `decide(t0 + 1005s) == DeclareLost`, `silenceLimit() == 5s`.
  - `"an answer after a wake restores the normal limits"`: as above, then `noteInbound(t0 + 1001s)`; `silenceLimit() == 15s`; `decide(t0 + 1005s) == None`; `decide(t0 + 1006s) == SendHeartbeat`.
  - `"an on-time tick changes nothing"`: `noteTick(t0 + 1s, t0 + 1s + 100ms)`; `decide(t0 + 2s) == None`.
  - `"an announced sleep silences the verdict until the wake"`: `noteSleep(t0 + 1s)`; `decide(t0 + 500s) == None`; `noteWake(t0 + 500s)`; `decide(t0 + 500s) == SendHeartbeat`.
  - `"a sleep announced and never taken does not disable liveness"` (Review Focus 1): `noteSleep(t0 + 1s)`; on-time ticks `noteTick(t0 + Ns, t0 + Ns)` for N = 2..7; `decide(t0 + 7s) == SendHeartbeat` (left `Asleep` at N = 6, entered `Waking`).
  - The existing case stays unchanged and passing.
- [ ] **Step 2:** Run `vthost_test "[liveness]"` → compile failure.
- [ ] **Step 3:** Implement as specified in Interfaces.
- [ ] **Step 4:** Run `vthost_test "[liveness]"` → PASS.
- [ ] **Step 5:** Commit `vthost: a frozen client probes the daemon instead of giving it up`.

### Task 7: `NativeClient` feeds the tick timing and the OS hint

**Files:**
- Modify: `src/vthost/client/NativeClient.hpp`, `src/vthost/client/NativeClient.cpp` (`armLivenessTick`, `livenessTick`)
- Test: `src/vthost/client/NativeClient_test.cpp` (the `ManualClockAttach` harness)

**Interfaces:**
- Consumes Task 6.
- Produces (reactor thread only, documented as such):
  ```cpp
  /// The system is about to sleep: no liveness verdict until it wakes.
  void noteSystemSleep();
  /// The system woke: the daemon is probed now and must answer within LivenessPolicy::wakeDeadline.
  void noteSystemWake();
  ```
  `armLivenessTick()` records `_livenessTickDue`; `livenessTick` calls `_liveness.noteTick(_livenessTickDue, now)` before deciding. Extract the switch into private `[[nodiscard]] LivenessOutcome applyLivenessDecision(core::platform::SteadyTimePoint now)` with `enum class LivenessOutcome : uint8_t { Continue = 0, Closed = 1 }` (Closed: the tick must not re-arm). Both `noteSystemSleep()` and `noteSystemWake()` return immediately unless liveness is live — the same condition that arms the tick: not `_closing`, not `_detached`, `_negotiated`, and `shares(Capability::Heartbeat)` (a declared-lost client is still current until Reconnecting arrives; a daemon without Heartbeat must never be sent one). `noteSystemWake()` then calls `_liveness.noteWake(now)` and `std::ignore = applyLivenessDecision(now)`. Loss reason: the existing message, with `_liveness.silenceLimit()` in place of `policy().deadAfter`.
  `heartbeatClient()` in the test sets `.awakeGap = 5s, .wakeDeadline = 5s` explicitly.

- [ ] **Step 1: Write failing tests** (tag `[vthost][attach][liveness]`), each with `ManualClockAttach`:
  - `"a client frozen past the deadline probes its daemon instead of dropping it"`: answering daemon — a script that echoes each `Heartbeat` (reuse the existing echoing script if one exists, else add `echoingAfterHello()` beside `silentAfterHello()`). Connected; `h.clock.advance(1000s)` in one step (do **not** call `h.settle()` — it detaches the client); then `h.letTimePass(10)`. `CHECK_FALSE(h.clientEnded)`; a `Heartbeat` was seen after the jump (record `h.seen.size()` before the jump).
  - `"a daemon gone during a sleep is given up on within the wake deadline"` (Review Focus 4): `silentAfterHello()`; connected; `advance(1000s)`; `letTimePass(6)`; `CHECK(turnUntil(h.loop, [&h] { return h.clientEnded; }))`; `lossReason() == "no answer from the daemon for 5 s"`; `h.settle()`.
  - `"an announced sleep and wake keep the connection"`: echoing daemon; `client->noteSystemSleep()`; `advance(1000s)`; `std::ignore = turnUntil(h.loop, [] { return false; }, 20)`; `client->noteSystemWake()`; `letTimePass(10)`; not ended; a `Heartbeat` seen.
  - The existing `"a client whose daemon stops answering ends with a loss reason"` still passes (15 s message).
- [ ] **Step 2:** Run `vthost_test "[liveness]"` → FAIL (the first two: client ends with the 15 s reason / compile failure).
- [ ] **Step 3:** Implement per Interfaces.
- [ ] **Step 4:** Run `vthost_test "[liveness]"` and `ctest --preset=clang-asan -L daemon` → PASS.
- [ ] **Step 5:** Commit `vthost: a client probes after a late tick or an OS wake instead of reconnecting`.

### Task 8: The reconnect states the real size, and pane sizes are re-asserted

**Files:**
- Modify: `src/contour/remote/NativeController.hpp`, `src/contour/remote/NativeController.cpp` (`makeClient`, `adoptReconnect`, `flushGeometry`), `src/contour/remote/RemoteLayout.cpp` (end of the 6-parameter `applyRemoteLayout`)
- Test: `src/contour/remote/NativeController_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  /// GUI thread. After a reconnect, once the GUI reconciled a layout of the new connection: re-sends every
  /// bound pane's size once, even though no display geometry changed. No-op otherwise.
  void reassertGeometryAfterReconcile();
  ```
  New private members, all under `_geometryMutex`:
  - `std::optional<vtpty::PageSize> _lastStatedArea` — the last area stated to any connection (hello or `requestResize`); never reset, not even for a restarted daemon.
  - `std::optional<vtpty::PageSize> _helloArea` — the area this connection's hello stated; consumed by its first `flushGeometry()`.
  - `GeometryReassert _geometryReassert` with `enum class GeometryReassert : uint8_t { Idle = 0, Pending = 1 }`; set `Pending` in `adoptReconnect()`.
- Behaviour:
  - `makeClient()`: hello area = `composedClientArea()` if `_acceptedConnections > 0` and it has a value; else `_lastStatedArea`; else the profile size (today's expression). Record it in `_helloArea` and `_lastStatedArea`. Lock order `_mutex` then `_geometryMutex`; first confirm no caller of `makeClient` holds either (it is called from `ClientSupervisor::attempt` via the factory on the reactor thread).
  - `flushGeometry()`: if `_helloArea` is set, take it; when the composed area equals it, set `_lastReportedArea` to it and send no area, but re-send **all** panes (the daemon re-projected them on the hello). Whenever an area is sent, record `_lastStatedArea`.
  - `reassertGeometryAfterReconcile()`: reads `_geometryReassert` under a scoped `_geometryMutex` lock, **releases it**, and calls `flushGeometry()` only if `Pending` (calling it under the lock would self-deadlock and invert the `_mutex` → `_geometryMutex` order).
  - `flushGeometry()`, inside its existing locked block: if `_geometryReassert == Pending`, set `Idle` and force `resendAll = true` — independent of `_helloArea`, which an earlier flush (e.g. a status-line toggle firing the resize sink before the layout arrived) may already have consumed.
  - `applyRemoteLayout(... 6 params ...)` calls `controller.reassertGeometryAfterReconcile()` as its last statement.

- [ ] **Step 0: Give the fixture a profile size.** `makeNativeController` (`NativeController_test.cpp:110-118`) passes `std::nullopt` attach settings, so today's hello states no area and the daemon never re-projects on reconnect — the bug would not reproduce. Add an optional `attachSettings` parameter (defaulting to today's `std::nullopt` so other tests are unchanged); every case below passes settings with `.pageSize = cells(100, 30)`.
- [ ] **Step 0b: A ResizeRequest counter.** Add a read-side decorator to the fixture's native connection socket (beside `GatedSocket`, which decorates writes only) that feeds the plaintext bytes into the proto frame decoder (keeping partial frames across reads) and counts decoded client `ResizeRequest` PDUs in an atomic the test reads. Test code only.
- [ ] **Step 1: Write failing tests** (tag `[attach][controller][reconnect]`), modelled on `"attach sizes remote panes to the panes the GUI renders"` (DaemonFixture, TestApp, ScopedController, `clientArea()`/`remoteSize()` lambdas), each using the Step 0 profile size.
  - `"a reconnect states the window's real size in its hello"`: attach, reconcile, local pty `resizeScreen(cells(120, 40))`, wait `clientArea() == cells(120, 40)`. `daemon.holdNewConnectionsAtLayout(); daemon.dropConnections();` wait Reconnecting then Connected. While the layout is still held: `CHECK(clientArea() == cells(120, 40))`.
  - `"a second drop before the layout states the last area"`: as above, but after the first reconnect is Connected and before `releaseHeld()`, drop again; wait Connected; `CHECK(clientArea() == cells(120, 40))`.
  - `"a reconnect re-asserts a split pane's size once reconciled"`: the split setup of the model test; resize the left pane's pty to a width that differs from the 0.5-ratio estimate; wait `remoteSize(left) == local`. Drop connections (no hold); wait Connected and a settled layout; `applyRemoteLayout(...)`; `CHECK(waitUntil([&]{ return remoteSize(leftSession) == leftLocalSize; }))`.
  - `"a reconnect to a restarted daemon states the real size"` (Review Focus 5): `daemon.restartHost()` instead of `dropConnections()`; after Connected, `CHECK(clientArea() == cells(120, 40))`.
  - `"the hello's area is not sent again after the layout"`: count client-area `ResizeRequest`s the daemon receives across a reconnect + reconcile with an unchanged window; expect 0. If `DaemonFixture` has no frame tap, add a counter in its native connection handler (test code only).
- [ ] **Step 2:** Run the test target holding `NativeController_test.cpp` with `"[reconnect]"` → the first, second and fourth fail with `clientArea() == cells(100, 30)`; the third fails on the left pane (re-projected by ratio, never re-asserted); the fifth passes or fails depending on timing and is kept as a regression guard.
- [ ] **Step 3:** Implement per Interfaces.
- [ ] **Step 4:** Run the `[reconnect]` and `[attach]` tags, then `ctest --preset=clang-asan -L daemon` → PASS. Run the `[attach][controller]` tests under `clang-tsan` (cross-thread `_geometryMutex` use).
- [ ] **Step 5:** Commit `remote: a reconnect states the window's real size and re-asserts every pane`.

### Task 9: Re-vendor core-cpp and wire the OS hint

Starts once Task 5's release is published.

**Files:**
- Modify: `vendor/core-cpp/**` (only via the `vendor/README.md` command), `cmake/ContourThirdParties.cmake` (set `CORE_CPP_WITH_SYSTEMD` from `CONTOUR_WITH_SYSTEMD`, beside the other core-cpp options at ~454-457), top-level `CMakeLists.txt` (the option is defined at ~158; move the non-Linux/Flatpak force-OFF that today lives in `src/vtpty/CMakeLists.txt:15-18` up to before `include(ContourThirdParties)` at ~358, so core-cpp sees the final value), `src/contour/remote/NativeController.hpp/.cpp`, `src/contour/ContourGuiApp.hpp/.cpp`, `metainfo.xml` (release note via `/add-release-note`)
- Test: `src/contour/remote/NativeController_test.cpp`

**Interfaces:**
- Consumes Task 1 (`ISystemPowerMonitor`, `PowerSubscription`, `testing::FakeSystemPowerMonitor`), Task 7 (`noteSystemSleep/Wake`).
- `NativeController` takes `core::platform::ISystemPowerMonitor* powerMonitor` (nullable) as a constructor parameter, subscribes in the constructor, holds the `PowerSubscription` as a member; the handler `_reactor.post`s to `_client->noteSystemSleep()` / `noteSystemWake()` when `_client != nullptr`. Note at the member: `~NativeController()` stops and joins the reactor before members die, so a handler may still post onto the stopped loop — safe because `ReactorThread::post` only queues; member order is not what protects it.
- `ContourGuiApp` owns `std::unique_ptr<core::platform::ISystemPowerMonitor> _powerMonitor`, created once with `makeNativeSystemPowerMonitor()`; on error, log it (display/attach log category) and pass `nullptr`. Created before, destroyed after, the `NativeController`.

- [ ] **Step 1:** Re-vendor; `ctest -L lint` (`core-cpp-vendored-copy`) → PASS; full build → PASS. Commit `vendor: core-cpp 0.13.0`.
- [ ] **Step 2: Write failing tests:**
  - `"a controller subscribes to power events for its lifetime"`: fake monitor; construct controller → `subscriberCount() == 1`; destroy → `0`.
  - `"a wake while reconnecting is dropped"` (Review Focus 3): fake; attach; `refuseConnections()`; wait Reconnecting; `fake.emit(DidWake)`; drain; no crash; `serveAgain()`; reconnects.
  - `"a wake probes the daemon"`: fake; attach; connect `connectionStateChanged` to record every `connectionState()`; `fake.emit(WillSleep); fake.emit(DidWake)`; pump for 1 s; the recorded states contain no `Reconnecting`, and the daemon side received a `Heartbeat` (via the Task 8 read decorator, extended to count `Heartbeat` too).
- [ ] **Step 3:** Run → FAIL (constructor signature).
- [ ] **Step 4:** Implement per Interfaces; update every `NativeController` construction (tests pass `nullptr` unless testing power).
- [ ] **Step 5:** Run the tests → PASS; configure with `CONTOUR_WITH_SYSTEMD=OFF` → builds.
- [ ] **Step 6:** `/add-release-note` (one entry: daemon-hosted panes no longer shrink to the profile size after the machine wakes). Commit `contour: probe the daemon on system wake instead of reconnecting`.

### Task 10: Finish

- [ ] **Step 1:** `/simplify`, then `/code-review`; address every finding.
- [ ] **Step 2:** `cmake --build --preset clang-asan && ctest --preset=clang-asan` → all pass; `ctest --preset=clang-tsan -L daemon` → pass.
- [ ] **Step 3:** Manual on macOS: a daemon-hosted tab running Claude Code at full window size; close the lid > 30 s; open. Layout stays full size. With the `vthost` log category raised, no "Lost the connection" line.
- [ ] **Step 4:** `/create-pr` with performance impact (none: one timestamp per 1 s tick), risk assessment, coverage.
