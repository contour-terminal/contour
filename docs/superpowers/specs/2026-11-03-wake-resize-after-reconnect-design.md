# Daemon-hosted panes keep the profile size after a wake — design

Date: 2026-11-03. Spans two repositories: core-cpp (a new API, released) and Contour (the fix, on the
re-vendored release).

## The bug

On macOS, after the lid is closed and opened again, a daemon-hosted pane shows its program laid out at
the profile's `terminal_size` (100×30 here) in the top-left corner of a full-size grid. Changing the
font size once fixes it.

Root cause, three links:

1. **A sleep reads as a dead daemon.** `vthost::client::LivenessTracker` (`src/vthost/client/Liveness.hpp`)
   declares the connection lost after `deadAfter` (15 s) of silence, measured with
   `core::platform::SteadyTimePoint` = `std::chrono::steady_clock`. On macOS libc++ implements that clock
   as `clock_gettime(CLOCK_MONOTONIC_RAW)`, which **counts sleep** (`man clock_gettime`), so the first 1 s liveness tick after a 1010 s sleep
   declares the connection lost and the supervisor reconnects. (Linux's `CLOCK_MONOTONIC` excludes
   suspend, which is why this was only seen on macOS.)
2. **A reconnect states the wrong size.** `NativeController::makeClient()`
   (`src/contour/remote/NativeController.cpp`) puts the *profile's* page size in every
   `ClientHello::clientArea`, the reconnect's included. With `size_policy: latest` the daemon adopts it
   and re-projects every pane to 100×30; the child gets SIGWINCH and redraws small.
3. **Nothing corrects it.** `adoptReconnect()` and `onLayout()` reset `_lastReportedArea` /
   `_lastReportedPaneSizes`, but deliberately issue no report; one is only sent when a display's
   geometry changes. After a wake none does, so the daemon keeps 100×30 until a font change forces
   a geometry pass.

Evidence: the screenshot's content block is exactly 100×30 cells while the indicator status line spans
the window; `pmset -g log` shows `Wake ... 15:56:03` after a 1010 s sleep; the daemon's command line
carries `--size-policy=latest`. Link 1 is inferred from the code and the timeline (neither process
logs reconnects at the default level); links 2 and 3 are read from the code.

## Goals

- A daemon-hosted pane is never left at a size other than the one its display shows, after any
  reconnect — wake, daemon hiccup, ssh drop.
- A reconnect does not make the daemon re-project to a wrong size first and the right one second (no
  double SIGWINCH). The hello states the real size.
- Opening the lid does not drop a healthy connection, local or remote.
- A remote daemon that really died while the machine slept is noticed within a few seconds of the wake.
- A reusable, injectable system sleep/wake notification API in core-cpp, used by Contour as a hint.

## Non-goals

- Delaying or inhibiting system sleep (logind inhibitor locks, deferring `IOAllowPowerChange`). The
  API notifies; it does not hold the system up.
- Changing the event loop's clock in core-cpp, or any other timer than the liveness check.
- Reworking the daemon's size policies.
- Logging reconnects at the default level (useful, but separate).

## Anti-goals

- A wake that forces an ssh re-authentication on a connection that was still alive.
- Liveness depending solely on the OS hint: a VM pause, `SIGSTOP`, a debugger stop and a Linux system
  without logind give no notification, and the check must still not misfire there.
- A new *mandatory* third-party dependency.

## Decisions

Decided by the user:

- **The OS sleep/wake hint is in scope now, as a core-cpp API** that Contour consumes.
- **Linux backend: logind's `PrepareForSleep` via sd-bus, an optional `libsystemd` dependency, default
  ON.**
- **The hello carries the real size on (re)connect**, rather than the profile size followed by a
  correcting resize.
- **A remote daemon must still be detected as dead promptly after a wake** — hence a wake probe with a
  short deadline rather than simply not counting sleep.
- **Split panes may see one extra resize after a reconnect.** The hello carries one client area, and the
  daemon re-projects every split from its ratios on accepting it (`SessionHost::applyClientSize` →
  `reprojectTab`); a pane whose real grid differs from that estimate is corrected by Part C's per-pane
  re-assertion, i.e. a second SIGWINCH. Accepted for this fix; unsplit tabs get exactly one correct
  resize. Per-pane sizes in the hello (a new capability) is a tracked follow-up, out of scope here.

### Part A — core-cpp: `core::platform` system power notifications

Builder's call (Claude's proposal, the user may revise):

- New header `src/core/platform/SystemPower.hpp`:
  ```cpp
  /// A system power transition the OS announces.
  enum class SystemPowerEvent : std::uint8_t
  {
      WillSleep = 0, ///< The system is about to sleep (or hibernate).
      DidWake = 1,   ///< The system resumed from sleep.
  };

  /// Notifies of system sleep and wake. The DI seam; the native monitor and a test fake implement it.
  class ISystemPowerMonitor
  {
    public:
      using Handler = std::function<void(SystemPowerEvent)>;
      virtual ~ISystemPowerMonitor() = default;
      /// @return A subscription that unsubscribes on destruction.
      [[nodiscard]] virtual Subscription subscribe(Handler handler) = 0;
  };

  /// @return The platform's monitor, or why there is none (no backend, logind unreachable, ...).
  [[nodiscard]] std::expected<std::unique_ptr<ISystemPowerMonitor>, PlatformError>
  makeNativeSystemPowerMonitor();
  ```
  `Subscription` is a move-only RAII token (reuse an existing core-cpp one if there is one; otherwise
  define it here). Unsupported platforms and a build without libsystemd return
  `PlatformError::NotImplemented`; a backend that fails to register returns a fitting error (add an
  enumerator at the end of `PlatformError` only if none fits).
- **Threading:** handlers run on a thread owned by the monitor, never the caller's; consumers marshal
  to their own loop. Documented on `subscribe()`. Subscribing and unsubscribing are thread-safe, and
  destruction of a subscription waits out a handler running for it.
- **Acknowledgement:** on macOS, `IOAllowPowerChange` is called after the handlers for `WillSleep`
  return; handlers must be quick and must not block. `kIOMessageCanSystemSleep` is allowed
  immediately and not surfaced.
- Backends:
  - **macOS** (`darwin/`): `IORegisterForSystemPower` with `IONotificationPortSetDispatchQueue` on a
    private serial dispatch queue. `kIOMessageSystemWillSleep` → `WillSleep`,
    `kIOMessageSystemHasPoweredOn` → `DidWake`. Links IOKit; no AppKit, so headless consumers work.
  - **Linux** (`linux/`): `sd_bus_open_system`, match `org.freedesktop.login1.Manager.PrepareForSleep`,
    on a private thread running `sd_bus_wait`/`sd_bus_process`, woken for shutdown (e.g. via an eventfd
    or `sd_bus_close` from the destructor). `true` → `WillSleep`, `false` → `DidWake`.
  - **Windows** (`windows/`): `PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, ...)`;
    `PBT_APMSUSPEND` → `WillSleep`, `PBT_APMRESUMEAUTOMATIC` → `DidWake` (ignore the duplicate
    `PBT_APMRESUMESUSPEND`).
- **CMake:** `option(CORE_CPP_WITH_SYSTEMD "Notify of system sleep/wake on Linux through logind (libsystemd)" ON)`
  in `cmake/CoreCppOptions.cmake`, Linux only; found via `pkg-config libsystemd`. ON but not found is a
  configure error, so a packager turns it off deliberately rather than silently losing the backend.
  - *Claude's call:* core-cpp's Linux CI legs (`.github/workflows/build.yml`, every `apt-get install`
    and the container leg) gain `libsystemd-dev`; at least one leg builds with the option OFF so the
    `NotImplemented` path stays compiled and tested.
  - The PRIVATE link to libsystemd must survive `CoreCppInstall.cmake`'s export check: add a
    `core_cpp_dependency` row or a small Find module, as that file requires (builder's call which).
  - *Claude's call:* Contour sets `CORE_CPP_WITH_SYSTEMD` from its existing `CONTOUR_WITH_SYSTEMD`
    (`cmake/ContourThirdParties.cmake`, beside the other core-cpp options). That option already pulls
    libsystemd-dev via `install-deps.sh` and is forced OFF inside Flatpak, so the Flatpak build loses
    the hint (no `org.freedesktop.login1` talk-name needed) and relies on the late-tick check.
- **Testing:** a `testing/` fake (`FakeSystemPowerMonitor` with `emit(SystemPowerEvent)`) for consumers;
  unit tests for subscription lifetime and dispatch through the fake; a native smoke test that
  `makeNativeSystemPowerMonitor()` constructs (or reports `NotImplemented`) and destroys cleanly.
  Real sleep is not exercised in CI.
- Documented in core-cpp's module docs and changelog; released as the next minor, then re-vendored into
  Contour with the documented one-command procedure (`vendor/README.md`). `vendor/core-cpp` is never
  edited by hand.

### Part B — Contour: liveness survives a sleep

In `vthost::client` (Contour-owned), independent of Part A so it covers the no-notification cases:

- **Late-tick detection.** `LivenessTracker` remembers when it last decided. A `decide(now)` arriving
  more than `LivenessPolicy::awakeGap` (5 s; the tick is 1 s) after the previous one means the process
  was frozen, not that the daemon was silent: it restarts the silence count from `now`, and asks for a
  probe at once (`SendHeartbeat`) instead of `DeclareLost`.
- **Wake deadline.** While that wake probe is out, the connection is declared lost after
  `LivenessPolicy::wakeDeadline` (5 s), measured from the wake, rather than `deadAfter` (15 s). Only the
  wake probe's echo ends the wake state: a frame buffered before the sleep proves nothing.
- **Which clocks reveal a sleep.** On macOS the steady clock counts the sleep (libc++ reads
  `CLOCK_MONOTONIC_RAW`), so a late tick reveals it; on Linux and the BSDs it excludes suspend, so there
  is nothing to detect and no false loss either. A late tick otherwise catches `SIGSTOP` or a VM pause.
- **The OS hint**, from Part A: `NativeClient` gains `noteSystemSleep()` / `noteSystemWake()`, reactor
  thread only. Sleep stops the liveness tick from deciding (so no verdict can race the wake); wake
  enters the same wake state as a detected late tick and sends the probe immediately. The late-tick
  detection stays as the fallback.
- **Wiring:** the composition root creates the monitor with `makeNativeSystemPowerMonitor()` (logging
  and continuing without it on error) and injects an `ISystemPowerMonitor*` (nullable) into
  `NativeController`; the controller subscribes and posts each event to its reactor, which forwards it
  to the current `NativeClient`. There is one composition root, `ContourGuiApp`
  (`src/contour/ContourGuiApp.cpp`); `contour client` opens a GUI window and goes through it too.
  `ReactorThread::post` is thread-safe, so marshalling from the monitor's thread needs nothing more.

### Part C — Contour: the real size, stated and kept

- **Hello.** `makeClient()` puts the real client area in `ClientHello::clientArea` once one is known:
  `composedClientArea()` when `_acceptedConnections > 0` and it yields a value; else the **last area
  this client stated** (hello or ResizeRequest), kept in a new member that no reset clears — a second
  drop before the new run's first layout push leaves `_layouts` empty, and falling back to the profile
  there would reintroduce the bug; the profile size only when nothing was ever stated (first connect).
- **No duplicate area after the hello** (*Claude's call*, the builder check found the original wording
  contradicted by the code): `adoptReconnect()` and `onLayout()` both reset `_lastReportedArea` after
  the hello is accepted, so recording the hello's area there is not enough. Instead the hello's area is
  held in a one-shot `_helloArea`, consumed by the first `flushGeometry()` of that connection: if the
  composed area equals it, the area is not resent (panes still are). The existing resets keep their
  meaning for the multi-client case they exist for; any later flush behaves as today.
  - `makeClient()` runs on the reactor thread; it takes `_mutex` then `_geometryMutex`, the documented
    order. The builder confirms no caller holds either already.
  - For a restarted daemon, `adoptReconnect()` clears `_paneSizes`; the hello is built before that, and
    the area it composes is still the window's real one, which is right for the new run too.
- **Per-pane re-assertion.** After a reconnect, once the new connection's layout has been reconciled by
  the GUI, `flushGeometry()` is run once even though no display geometry changed, so every bound pane's
  size (split refinements included) is re-sent. The area is not resent if it equals what the hello
  stated.
  - *Builder's call:* the trigger point — the GUI-side reconciliation of the first layout (batch) after
    `adoptReconnect()`, so pane extents are laid out against the new tree (the reason `onLayout()`
    deliberately issues no report today).

## Testing

- `Liveness_test`: a tick 1000 s late yields `SendHeartbeat`, not `DeclareLost`; silence after that
  probe is lost at `wakeDeadline`; an inbound frame clears the wake state; a normal 1 s cadence is
  unchanged.
- `NativeClient_test`: `noteSystemSleep()` then a long gap then `noteSystemWake()` keeps the connection
  and sends a Heartbeat; a wake with no answer is lost within `wakeDeadline`.
- `NativeController_test`: after a reconnect, the hello's `clientArea` equals the composed real area,
  not the profile size; the per-pane sizes are re-sent once after the layout; no duplicate area
  ResizeRequest.
- core-cpp: as in Part A.
- Manual: macOS, lid closed for >15 s and reopened with a Claude Code session in a daemon-hosted tab —
  layout stays at the full size, no reconnect logged (raise the `vthost` log category to see it).

## Order

1. core-cpp: Part A, PR, release (minor).
2. Contour: re-vendor; Parts B and C with their tests on `fix/wake-resize-after-reconnect`; one PR.
   Parts B (late tick) and C need nothing from core-cpp and may land first if the release lags.

## Left to the builder

- Naming inside the bounds above, the `Subscription` type's home, which `PlatformError` a failed
  registration maps to.
- The exact reconciliation hook for Part C's re-assertion, and the composition-root owner of the
  monitor.
- Default values `awakeGap = 5 s` and `wakeDeadline = 5 s` are Claude's call; adjust if tests show a
  reason.
