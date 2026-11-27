### Task 5.1: The sandbox seam and the lockdown order (no OS mechanism yet)
**Files:**
- Create: `host/Sandbox{.hpp,.cpp,_none.cpp}` (`_none.cpp` is compiled where no platform file exists yet), `host/Sandbox_test.cpp` (remote_test).
- Modify:
  - `remote/proto/{Pdu.hpp,PduTable.hpp,Codec.cpp,Codec_test.cpp}`.
  - `host/{HostBootstrap.hpp,HostBootstrap_posix.cpp,HostBootstrap_win32.cpp,HostProcess.hpp,HostProcess.cpp,HostProcess_test.cpp,HostServer.hpp,HostServer.cpp,HostServer_test.cpp,IHostedEngine.hpp,CMakeLists.txt}`.
  - `wasmtime/{WasmtimeEngine.hpp,WasmtimeEngine.cpp,HostedEngine.cpp,WasmtimeEngine_test.cpp}`, `testing/HostedEngine.hpp`.
  - `remote/{HelperIncarnation.cpp,HostRefusals.hpp,HostRefusals.cpp,HostRefusals_test.cpp}`, `process/{ProcessHostConnector.hpp,.cpp}`, `process/testing/FakeProcessLauncher`, the supervisor/connector tests.
  - `src/contour-extension-host/testing/HostileHost.cpp`.

**Produces:**
- Proto:
  - `ProtocolVersion = 4`.
  - `LockdownStep : uint8_t { None, CoreDumps, Dumpable, Tracing, FaultReports, NoNewPrivileges, FileSystem, SystemCalls, ThreadSync, ResourceCaps, Capabilities, DescriptorRights, Profile, Container, Mitigations }`. The hardening steps fold in here (D87).
  - `Hello` gains `LockdownStep failedStep` and `uint32_t failureCode` (errno or HRESULT bit pattern).
- `lockdownSteps()` rows `{step, text}` in `remote_wire`; both sides phrase a step from it.
- `ConfinementFailure{LockdownStep step; int code}` replaces `HardeningFailure`. `hardenProcess()` returns `expected<void, ConfinementFailure>`.
- `host::ISandbox`:
  - `restrictBeforeThreads() -> expected<void, ConfinementFailure>` (what threads inherit);
  - `lockDown() -> expected<void, ConfinementFailure>` (after every thread started and was prepared).
- `SandboxDescriptors{NativeHandle control}`.
- `makeSystemSandbox(SandboxDescriptors) -> unique_ptr<ISandbox>`. `NullSandbox` reports `Unsandboxed` and is what `_none.cpp` returns.
- `IHostedEngine::prepareThread() -> expected<void, ExtensionError>`.
- `HostConfinement` gains `sandbox` (`function<unique_ptr<ISandbox>(SandboxDescriptors)>`). `confine` becomes caps then `lockDown`.
- `ProcessHostConnector` remembers a `LockdownFailed` refusal (D90).

- [ ] **Step 1: Write the order tests** (remote_test, scripted parent, recording fakes).
  - `"HostProcess.locksDownInTheSandboxOrder"`. The recorded sequence is exactly: bind, harden, adopt, engine, restrict, listener started, 4 executors started, 5 × prepareThread (4 lanes + control), caps, lockDown, Hello sent. `restrict` saw 0 threads started.
  - `"HostProcess.everyConfinementStepRunsPastAFailure"`: restrict fails `FileSystem` and lockDown fails `SystemCalls`. Both run. Hello is `{Failed, FileSystem, code}`, and one `sandbox failed: <text>: <system text>` line is told.
  - `"HostProcess.hardeningFailureIsStatedAsItsStep"`: a fake hardening row fails `Dumpable` → Hello `{Failed, Dumpable, code}`.
  - `"HostProcess.nullSandboxSaysUnsandboxed"`: Hello `{Unsandboxed, None, 0}`, which is today's behaviour.
- [ ] **Step 2: Write the server and engine tests.**
  - `"HostServer.everyThreadIsPreparedBeforeConfinement"`: a counting engine. At `confine`, prepareThread calls == `MaxLaneChannels + 1`, each on a distinct thread id, and the control thread is among them.
  - `"HostServer.unpreparedThreadEndsTheHost"`: lane 2's prepareThread fails → `ServerExit::ExecutorUnavailable`, confine never runs, nothing is sent.
  - `"WasmtimeEngine.prepareThreadRunsTheGuestPath"`: Software and Signals (counts as one of the two Signals cases, through `EngineSignalHandlers`). Returns ok, is idempotent per thread, and loads its built-in module once per engine.
- [ ] **Step 3: Write the wire, refusal and connector tests.**
  - `"PduCodec.protocolVersionIsFour"`. The Hello round-trip samples gain every `LockdownStep`. Over-cap and mutation cases cover the row.
  - `"LockdownSteps.oneRowPerStep"`: the texts name no engine.
  - `"HostRefusals.lockdownLineNamesTheStep"`: `refusalLine(LockdownFailed, step)` contains the step's text.
  - `"ProcessHostConnector.lockdownFailureIsNotRelaunched"` (FakeProcessLauncher): the first helper says `Failed` → `HostUnavailable`, and the line is logged once. Three later connects → `HostUnavailable` with `launches == 1`.
- [ ] **Step 4: Build** `vtextension_remote_test vtextension_process_test vtextension_wasmtime_test`; expect failure.
- [ ] **Step 5: Implement the wire row and the step table** (D89). Rename `HardeningStep` → `LockdownStep` throughout, and `HardeningFailure` → `ConfinementFailure`.
- [ ] **Step 6: Implement the seam and the order** (D86, D87).
  - `runExtensionHost`: bind → harden → adopt → engine → `sandbox(descriptors)->restrictBeforeThreads()` → `HostProcess` (listener, then executors) → prepare → caps → `lockDown()` → Hello.
  - Every step runs, and the first failure is the one stated.
  - The hostile host passes a `NullSandbox` factory (D97).
- [ ] **Step 7: Implement priming** (D88).
  - Executors call `prepareThread()` first and count down a latch. `HostServer::run()` prepares its own (control) thread, then waits on the latch, then runs `confine`.
  - Wasmtime: compile a built-in `(module (func (export "p")))` once per engine, then instantiate and call it per thread.
  - The copying engines and the test doubles: no-op.
- [ ] **Step 8: Implement the sticky refusal** (D90) and the refusal line.
- [ ] **Step 9: Run** `remote_test "HostProcess.*" "HostServer.*" "PduCodec.*" "LockdownSteps.*" "HostRefusals.*" "Sandbox.*"`, `process_test "ProcessHostConnector.*" "Supervisor.*"` and `wasmtime_test "WasmtimeEngine.*"` → PASS. Then `helper_test "~[.measure]"` whole → PASS (Unsandboxed; hostility unchanged); ctest → PASS.
- [ ] **Step 10: Commit** (three):
  - `refactor(vtextension): one table names every step of the extension host's confinement`
  - `feat(vtextension): the extension host locks down in a fixed order, its threads prepared first`
  - `feat(vtextension): say which confinement step of an extension host failed, and start it only once`


### Controller notes — decisions and rulings that bind this task (verbatim from the plan)
- D85. Eight tasks: 5.1 seam and order → 5.2 Linux → 5.3 WAMR helper → 5.4 Linux default → 5.5 FreeBSD, 5.6 macOS, 5.7 Windows → 5.8 gate. The outline's 5.1 splits into the seam (5.1) and Linux (5.2), and WAMR moves before the default switch. — The order, priming and refusal path are platform-free and testable with fakes. A Linux build without Wasmtime must have a helper the moment the default flips.
- D86. `host::ISandbox` has two steps: `restrictBeforeThreads()` (what threads inherit: Linux no_new_privs + Landlock; FreeBSD `PROC_NO_NEW_PRIVS`) and `lockDown()` (after every thread is prepared: seccomp TSYNC, `cap_enter`, Seatbelt, the Windows verification).
  - Made by `makeSystemSandbox(SandboxDescriptors)`.
  - `NullSandbox` → `Unsandboxed`.
  - Every step runs, and the first failure is stated, as D66 does for hardening.

  — Spec §5.6's order has two points, and only one of them is per thread.
- D87. One step vocabulary: `HardeningStep` folds into `proto::LockdownStep`, and `HardeningFailure` becomes `ConfinementFailure`. `runExtensionHost` order: bind → harden → adopt → engine → restrict → listener + executors → prepare → caps → lockDown → Hello. Caps run before seccomp. — One table phrases every confinement failure. Caps first means `setrlimit` needs no row.
- D88. Priming is `IHostedEngine::prepareThread()`, on every executor and on the control thread (which runs start functions, R29), behind a latch before `confine`. A failure → `ExecutorUnavailable`.
  - Wasmtime instantiates and calls a built-in one-function module. That installs its lazily-installed handlers and the thread's sigaltstack, so `rt_sigaction` needs no row.
  - WAMR initializes its thread environment.
  - The listener thread runs no guest, so it is not primed.

  — Phase 4 measured lazy handler installation; priming makes the post-lockdown syscall set deterministic.
- D89. Hello states the failed step and code (`failedStep`, `failureCode`): `ProtocolVersion = 4`. The parent's `LockdownFailed` line names the step from the shared table. — Users on unusual kernels and in Flatpak get a reason. Release helpers' stderr reaches nobody.
- D90. **Deviation, needs a ruling.** A `LockdownFailed` refusal is sticky in its `ProcessHostConnector`: later connects fail `HostUnavailable` without launching, and the breaker counts faults as usual. It is not "open the breaker at once" (spec §5.3; D52 assigned it here). — A lockdown failure is a property of the system and build, not of an extension. This needs no new error across the seam. Cost: it stays refused until the backend is recreated (restart), even if the cause was transient (ENOMEM).
- R39. D85–D110 are accepted, except as changed below.

- Path convention of this phase: paths are relative to `src/vtextension/wasm/` unless they start with `src/`, `cmake/`, `scripts/`, `support/`.
