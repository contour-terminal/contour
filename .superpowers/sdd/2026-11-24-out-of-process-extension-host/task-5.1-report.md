# Task 5.1 report: the sandbox seam and the lockdown order

Status: DONE_WITH_CONCERNS (concerns at the end; none blocks 5.2).

Commits (on `feature/out-of-process-extension-host`, from `009b13c5e`, not pushed):

- `9f8b0b151` refactor(vtextension): one table names every step of the extension host's confinement
- `e075e85a1` feat(vtextension): the extension host locks down in a fixed order, its threads prepared first
- `fd96231ad` feat(vtextension): say which confinement step of an extension host failed, and start it only once

## What was done

### Wire (D89)
- `proto/Pdu.hpp`: `ProtocolVersion = 4`; `enum class LockdownStep : uint8_t` with the brief's 15 values verbatim;
  `Hello` gains `LockdownStep failedStep = None` and `uint32_t failureCode = 0`.
- `proto/Codec.cpp`: `isEnumerator(LockdownStep)` switch; `members(Hello)` carries the two fields. Hello's row cap
  stays 16 (largest body is 13: 5 + 1 + 1 + 1 + 5), so `PduTable.hpp` is unchanged.
- New `proto/LockdownSteps.{hpp,cpp}` in `vtextension_remote_wire`: `LockdownStepRow{step, text}`,
  `lockdownSteps()`, `lockdownStepText()` (unknown value → the `None` row). Indexed table with a `static_assert`.

### Seam and order (D86, D87)
- New `host/Sandbox.{hpp,cpp}`, `host/Sandbox_none.cpp`: `ConfinementFailure{LockdownStep step; int code}`,
  `confinementFailureText()` (`"<step text>: <system text>"`), `SandboxDescriptors{NativeHandle control}`,
  `ISandbox{restrictBeforeThreads(), lockDown(), lockdownState()}`, `NullSandbox` (`Unsandboxed`),
  `makeSystemSandbox()` (Sandbox_none.cpp returns `NullSandbox`; `host/CMakeLists.txt` `_sandbox_source`).
- `HostBootstrap`: `HardeningStep`/`HardeningFailure`/`hardeningFailureText` removed; `HardeningRow{step, apply}`
  (text from the table); `hardenProcess()` and `capResources()` return `expected<void, ConfinementFailure>`
  (caps: `ResourceCaps` + errno, every cap applied past a refusal).
- `HostConfinement` = `{bindToParent, beforeThreads, sandbox, startThread, confine, tell}`. `runExtensionHost`:
  bind → harden → adopt → engine → `sandbox({control})` → `restrictBeforeThreads()` → `HostProcess::run`
  (listener, executors, priming) → `confine` = caps then `lockDown()` → Hello. Every step runs; the first failure
  of hardening/restrict/caps/lockDown is what Hello states. Told lines: `hardening failed: …`, `sandbox failed: …`
  (the sandbox's first failure only — one line), `resource caps failed: …`.
- `HostServerConfig::confine` now returns `expected<LockdownState, ConfinementFailure>`; the server maps a failure
  to `Hello{Failed, step, static_cast<uint32_t>(code)}`.
- `ExecutorStarter` moved to `ExecutorThread.hpp`; `InterruptListener::start` takes one; `HostProcess` starts the
  listener with `HostServerConfig::startExecutor`, so `HostConfinement::startThread` covers every thread.
- Hostile host: `systemConfinement()` with `sandbox` = a `NullSandbox` factory.

### Priming (D88)
- `IHostedEngine::prepareThread() -> expected<void, ExtensionError>` (pure virtual).
- `HostServer::run`: each executor calls `prepareThread()` first and counts down a `std::latch`; a failed one
  returns without serving. The control thread prepares itself after starting all executors, waits on the latch,
  and on any failure ends `ExecutorUnavailable` without confining or sending.
- Wasmtime (`WasmtimeEngine.cpp`): each facade compiles the built-in `(module (func (export "p")))` once, on an
  engine of its own (the facade's trap mode, `Private` memory, 64 KiB stack, epoch deadline 1), then per thread
  instantiates and calls it. A `thread_local` keeps the serial of the facade that primed the thread last
  (idempotent per thread). `wasmtime::testing::primingCompilations()` counts compiles.
- `DecoratedEngine` forwards; new `testing/PreparationHookEngine.hpp` (hook before the inner preparation) serves
  the HostServer/HostProcess tests.

### Sticky refusal (D90, R40)
- `IHostConnector::refused(HostRefusal)` (pure virtual). `HelperIncarnation::connect` calls it beside
  `HostRefusalLog::refused(refusal, failedStep)`.
- `ProcessHostConnector`: `std::atomic<LockdownHistory>`; a `LockdownFailed` refusal makes every later
  `connect()` fail `HostUnavailable` without launching and without a line (the backend's log said it).
  `InProcessHostConnector` keeps nothing; test connectors record or forward.
- `refusalLine(HostRefusal, LockdownStep)` / `HostRefusalLog::refused(HostRefusal, LockdownStep)`:
  "The extension host could not confine itself, and was refused: <step text>. Extensions do not run."
- `FakeProcessLauncher(LaunchHook)`: a hook run at launch with the spec (inherited handles open), so a test plays
  the host.

### Tests added / changed
- `vtextension_test`: `PduCodec.protocolVersionIsFour`, `PduCodec.helloAtItsLargestFitsItsCap`,
  `LockdownSteps.oneRowPerStep` (texts name no engine; every row decodes, one past the last is BadEnum); a Failed
  Hello sample with `UINT32_MAX` code in the round-trip/prefix/mutation samples; lying-body vectors updated.
- `vtextension_remote_test`: `HostProcess.locksDownInTheSandboxOrder`, `.everyConfinementStepRunsPastAFailure`,
  `.hardeningFailureIsStatedAsItsStep`, `.nullSandboxSaysUnsandboxed`, `.sandboxThatLocksDownSaysSo`,
  `.refusedCapIsStatedAsItsStep`; `HostServer.everyThreadIsPreparedBeforeConfinement`,
  `.unpreparedThreadEndsTheHost`; `Sandbox.*` (3); `ProcessHostConnector.lockdownFailureIsNotRelaunched`;
  `RemoteBackend.refusedHelloIsLoggedOncePerReason` now checks the step in the line and that the connector heard
  each refusal; HostBootstrap tests moved to the new types.
- `vtextension_process_test`: `HostRefusals.lockdownLineNamesTheStep`.
- `vtextension_wasmtime_test`: `WasmtimeEngine.prepareThreadRunsTheGuestPath` (sections Software / Signals; the
  Signals one checks the fresh thread gains a `sigaltstack`, uses `EngineSignalHandlers`, skips when instrumented).

## Deviations, with reasons

1. **`ProcessHostConnector.lockdownFailureIsNotRelaunched` is in `vtextension_remote_test`**
   (`process/LockdownRefusal_test.cpp`, added only where `_process_source`), not `process_test`: the brief's
   scenario has a helper *say* Failed, which needs `HelperIncarnation`, and `vtextension_process_test` does not link
   `vtextension_remote`. It runs through a real `ProcessHostConnector` + supervisor + `FakeProcessLauncher` whose
   hook plays the host on the inherited control handle. It also runs under wine.
2. **`PduCodec.*` and `LockdownSteps.*` run in `vtextension_test`**, where `Codec_test.cpp` already lives.
3. **The connector learns of the refusal through a new `IHostConnector::refused()`** (the brief does not say how).
   D90's "no new error across the seam" holds: `connect()` still only says `HostUnavailable`. The line is logged by
   the backend's `HostRefusalLog` as before (once), not by the connector, so Contour's log says it once.
4. **`HostConfinement` gained `startThread`** (and `InterruptListener::start` a starter): the order test must see
   the listener and executor starts. Not in the brief's file list beyond `HostProcess`.
5. **`.sandboxThatLocksDownSaysSo` and `.refusedCapIsStatedAsItsStep`** added beyond the brief, for
   `lockdownState()` and the caps' new step.
6. **The copying Wasmtime engine primes too** (brief: "copying engines … no-op"). It is the same class as the
   sharing one and runs guests (with Signals in release), so a no-op there would leave the copy-memory helper
   unprimed once 5.2 locks it down. Test doubles (`DecoratedEngine` forwards, `PreparationHookEngine`) as briefed.
7. **The parent's line names the step, not the code.** `systemErrorText` is helper-side (host lib); the parent
   could phrase errno with `std::system_category()`, but that would duplicate it. The code is on the wire for 5.x.
8. **"adopt" is not recorded as an event** in the order test (no seam); it is proven by the sandbox being made for
   the very control handle the host was handed (`sandboxedControl == arguments.control`).

## Tests and commands (all run; real counts)

Build (ASan): `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset
clang-asan -j 4 --target vtextension_test vtextension_wasmtime_test vtextension_process_test
vtextension_helper_test vtextension_remote_test contour-extension-host contour-extension-host-hostile
extension_host_probe` — clean, no warnings.

- Focused: `vtextension_remote_test "HostProcess.*,HostServer.*,Sandbox.*,HostBootstrap.*,ProcessHostConnector.*"`
  — 65 cases / 2037 assertions, passed, 3 runs in a row. `vtextension_test "PduCodec.*,LockdownSteps.*,
  PduStream.*"` — 31 cases / 19401 assertions. `vtextension_process_test "HostRefusals.*,ProcessHostConnector.*"`
  — 6 / 133. `vtextension_wasmtime_test "WasmtimeEngine.*"` — 18 cases, 15 passed, 3 skipped (Signals sections).
- Whole suites (ASan): vtextension_test 476 cases / 26205 assertions passed; vtextension_wasmtime_test 64 (61 + 3
  skipped) / 1157; vtextension_process_test 108 (107 + 1 skipped) / 857; vtextension_remote_test 301 / 12253;
  vtextension_helper_test `"~[.measure]"` 158 (157 + 1 skipped) / 3861 — Unsandboxed, hostility unchanged.
- `ctest --preset=clang-asan -R vtextension`: 5/5 passed (42.6 s).
- clang-tidy: `JOBS=4 scripts/clang-tidy-diff.sh --base HEAD` (new files `git add -N`'d) — one finding
  (`performance-unnecessary-value-param` in HostServer_test) fixed; then 28 TUs clean. `HostBootstrap_win32.cpp`
  is not in the Linux compile database (not linted).
- `/usr/bin/clang-format` (22.1.8) on every changed C++ file; `editorconfig-checker` on the changed files: clean.
- Windows (touched `HostBootstrap_win32.cpp`): cross-built `out/clangcl-debug` (clang-cl, `-Werror`)
  `vtextension_process_test extension_host_probe contour-extension-host contour-extension-host-hostile
  vtextension_helper_test vtextension_remote_test` — clean. wine 11.20, own prefix `scratchpad/wine51`,
  `WINEPATH` = mingw sysroot bin: process_test 101 (100 + 1 skipped) / 798; remote_test 272 (270 + 2 skipped) /
  11404 (includes the new HostProcess/HostServer/Sandbox/lockdown-refusal cases); helper_test `"~[.measure]"` 128
  (126 + 2 skipped) / 3035. `wineserver -k` on my own prefix only.

Not run: TSan (run rules: no other presets), release tree (so the Signals section of
`WasmtimeEngine.prepareThreadRunsTheGuestPath` was skipped everywhere it ran; see concerns), `/simplify` and
`/code-review` (they dispatch agents, which this task forbids; I de-duplicated the HostProcess tests by hand).

## Performance

No hot path changes: priming runs once per thread at host start, plus one engine creation and one compile of a
31-byte module per engine. Not measured (5.2 measures in a release tree); expected well under a millisecond
against the 1.28 ms spawn-to-runtime of phase 4. The priming engine stays alive for the host's life (a few hundred
KiB at most, unmeasured).

## Risk

- Low for behaviour today: every system still runs `Unsandboxed` through `NullSandbox`; the Hello shape changes
  with the version bump, which both ends share in one build (the experimental-feature rule allows breaking).
- The latch: a lane whose prepare fails returns without serving; the others serve until `finish()` shuts the
  channels. Covered by `HostServer.unpreparedThreadEndsTheHost` (ASan + wine); not run under TSan.
- D90's cost stands (R40): after a transient lockdown failure extensions stay off until the backend is recreated.

## Coverage

Not measured with the coverage preset (not allowed by the run rules). By test: every new branch in
`runExtensionHost` (each step failing alone or with another), the server's prepare failure, the connector's sticky
path, `refusalLine` with/without a step, codec over every step value, NullSandbox. Untested: a real `setrlimit`
refusal (the fork tests only prove success), `compilePriming`/`runPriming` failure branches (Wasmtime would have to
fail an empty module), and the Signals priming section in this tree.

## Concerns

1. **The three commits are split by file, so the first two do not build on their own**, and the Hello fields and
   version 4 landed with the first commit's `Pdu.hpp` though the third commit's message describes them. Only the
   final head (`fd96231ad`) was built and tested. Squash or re-split before merge if bisectability matters.
2. **The Signals priming check never ran** (ASan skips it; wine is a Windows build without `sigaltstack`). It
   asserts that a fresh thread gains a `sigaltstack` after `prepareThread()` under Signals — the claim D88 rests
   on. 5.2 should run `vtextension_wasmtime_test "WasmtimeEngine.prepareThreadRunsTheGuestPath"` in the release
   tree first; if Wasmtime 48 installs no alternate stack there, the assertion (not the priming) is what is wrong.
3. Phase-5 facts were added to the plan under `### Phase-5 facts` ("From 5.1"), right before the phase-5 Review
   Focus addendum.

## Fix round 1: re-split so each commit builds

Finding: `9f8b0b151` and `e075e85a1` did not build on their own, and the Hello fields and version 4 sat in the
first commit. I rewrote the three unpushed commits on base `009b13c5e` (detached HEAD in this worktree, then moved the branch
to the result). New commits:

- `1b276b1c4` refactor(vtextension): one table names every step of the extension host's confinement. Contains:
  `LockdownStep` in Pdu.hpp (with a doc comment that doesn't mention Hello), `proto/LockdownSteps.{hpp,cpp}` and their CMake
  lines, `Sandbox.hpp` reduced to `ConfinementFailure` + `confinementFailureText` with `Sandbox.cpp` and its CMake
  lines, HostBootstrap (all four files), and HostProcess adapted to the new types without a sandbox. In that commit,
  `confine` caps and tells `resource caps failed: …`, and Hello says `Failed` if hardening or caps failed. Tests: old
  HostProcess/HostBootstrap tests moved to the new types, plus a table-only `LockdownSteps.oneRowPerStep`. There is no
  version bump, no Hello field and no codec change.
- `d9ad0c8df` feat(vtextension): the extension host locks down in a fixed order, its threads prepared first.
  Contains the sandbox seam, the order, priming, Wasmtime, the hostile host and `Sandbox_test`, with no Hello fields: the
  server sends `Hello{…, lockdown = Failed}` on a failure, and the tests check the step through the told lines. Body
  adjusted to match: "Hello states Failed, and the host tells of the first step that failed".
- `8af97b4b3` feat(vtextension): say which confinement step of an extension host failed, and start it only once.
  Contains version 4, `Hello.failedStep`/`failureCode`, the codec, the wire half of `LockdownSteps.oneRowPerStep`,
  the server filling the fields, the step-checking test expectations, refusal lines, the sticky connector,
  `FakeProcessLauncher` hook and `LockdownRefusal_test`.

Per-commit build (ASan, `flock -o … nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4`):

| Commit | Targets | Build | Tests run |
|---|---|---|---|
| `1b276b1c4` | vtextension_test, remote_test, process_test, helper_test, contour-extension-host, -hostile, probe | clean, no warnings | remote_test `HostProcess.*,HostBootstrap.*` 17 cases / 257 assertions passed; vtextension_test `PduCodec.*,LockdownSteps.*` 14 / 19104 passed |
| `d9ad0c8df` | the above + wasmtime_test | clean, no warnings | remote_test `HostProcess.*,HostServer.*,Sandbox.*,HostBootstrap.*` 64 / 2021 passed; wasmtime_test `WasmtimeEngine.*` 18 (15 + 3 skipped) / 225 |
| `8af97b4b3` | the above | clean, no warnings | `ctest --preset=clang-asan -R vtextension`: 5/5 passed (42.3 s) |

`git diff fd96231ad HEAD` is empty (0 bytes), so the final tree is byte-identical to the one reviewed, the one
cross-built for Windows and the one run under wine. The intermediate commits were not cross-built for Windows
(`HostBootstrap_win32.cpp` is the same in all three; it compiles against the commit-1 `Sandbox.hpp`, a subset of
the final one). All commits carry `Signed-off-by` and no other trailers.
