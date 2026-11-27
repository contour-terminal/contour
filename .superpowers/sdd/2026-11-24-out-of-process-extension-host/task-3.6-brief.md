### Task 3.6: Crash and hostility suite

**Files:**
- Modify: `src/contour-extension-host/testing/{HostileModes.hpp,HostileHost.cpp}` (the remaining rows)
- Modify: `src/vtextension/wasm/remote/process/testing/RecordingLauncher.hpp` (argument rewrite for launches after the first, if 3.1 left it out)
- Create: `src/vtextension/wasm/remote/Hostility_test.cpp` (vtextension_helper_test)

**Interfaces:**
- Consumes: 3.3–3.5.
- Produces: `HostileMode` rows (name → action), each acting once per incarnation, through a relay that splits frames with `proto::decodeFrame` and forwards the raw bytes between the parent's channels and an inner `HostProcess`'s:

  | Mode | Acts when | Action |
  |---|---|---|
  | `abort` | the first lane `Call` arrives | `std::abort()` |
  | `segfault` | the first lane `Call` arrives | POSIX `std::raise(SIGSEGV)` with `SIG_DFL`; Windows `TerminateProcess(self, STATUS_ACCESS_VIOLATION)` |
  | `exit-in-callhost` | the inner server sends its first `CallHost` | forward it, then `_exit(3)` |
  | `stop-reading` | the first lane `Call` arrives | stop reading every parent channel; stay alive |
  | `garbage` | the first lane `Call` arrives | write 64 bytes from `std::mt19937{ 0x5eed }` to that lane instead |
  | `over-cap` | the first `MemoryRead` | answer a `MemoryData` header declaring `maxMemoryBytes + 17` body bytes |
  | `bogus-size` | the first `MemoryRead` | answer a well-formed `MemoryData{ Ok }` with `size - 1` bytes |
  | `stall-after-header` | the first `MemoryRead` | send a valid `MemoryData` header and half its body, then stall |
  | `hang-at-handshake` | start | hold back `Hello` |
  | `hang-at-load` | the first `Load` | hold back `Loaded` |
  | `hang-at-instantiate` | the first `Instantiate` | hold back `Instantiated` |
  | `ignore-interrupt` | (3.4) | |
  | `stop-reading-after-instantiate`, `ignore-eof` | (3.5) | |

- [ ] **Step 1: Write the suite** (`Hostility_test.cpp`). One expectations table, one `DYNAMIC_SECTION` per row; each section builds a fresh supervisor (`teardownGrace = 2s`), a `RecordingLauncher` whose launches after the first drop `--hostile` (so the respawn is benign), `timeouts = { load 200ms, instantiate 200ms, handshake 200ms, interruptGrace 50ms }`, `AcceptUnsandboxed`; a "watchdog" `std::jthread` interrupts the instance 50 ms after a call or fetch starts, as the pool's watchdog would.

  | Mode | Operation (InstanceModule) | Expected | Bound |
  |---|---|---|---|
  | `abort`, `segfault`, `garbage`, `stop-reading` | `call("add", {2, 3})` | `CallFailure::HostCrashed` | 2 s |
  | `exit-in-callhost` | `call("ping-then-add", {2, 3})` | `HostCrashed` | 2 s |
  | `over-cap`, `bogus-size`, `stall-after-header` | `memory(16, 5)` on a prepared thread | `MemoryFailure::HostCrashed` | 2 s |
  | `hang-at-handshake`, `hang-at-load` | `runtime->load(...)` | `ExtensionError::HostUnavailable` | 2 s |
  | `hang-at-instantiate` | `module->instantiate(ctx)` | `HostUnavailable` | 2 s |

  Each section asserts: the expected failure; elapsed (steady clock) < bound; exactly one death before recovery — `counters().deathsObserved == 1` (a supervisor kill is reaped and observed like any exit) and `recording.launches() == 1`; `uncleanExits` equals 1 for `abort`/`segfault`/`exit-in-callhost` (the helper died on its own) and 0 for the rest (the supervisor's kill); lazy recovery — the same module (for the load rows: a new load) then instantiates and `add(2, 3) == 5`, with `recording.launches() == 2`.
- [ ] **Step 2: Write the breaker cases** (Instance level, through `ExtensionRegistry` and `Engine` over this composition, the statusline test module): `"Hostility.oneCrashIsOneFault"` (`abort`: `render(...) == HostUnavailable`, `breaker.faultCount() == 1`, the next `Instance::create` renders); `"Hostility.oneHangIsOneFault"` (`hang-at-instantiate`: `Instance::create(...).error() == HostUnavailable`, `faultCount() == 1`); `"Hostility.oneViolationIsOneFault"` (`garbage`); `"Hostility.repeatedCrashesOpenTheBreaker"` (`abort` on every launch, no rewrite: three create+render rounds → `faultCount() == 3`, `state() == Open`; a fourth `Instance::create` → `HostUnavailable` with `recording.launches() == 3`); `"Hostility.hangingLoadOpensTheBreaker"` (D53: three `acquire`s under `hang-at-load` → a fourth fails without launching).
- [ ] **Step 3: Build; expect failures.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target contour-extension-host-hostile vtextension_helper_test`.
- [ ] **Step 4: Implement the rows** (the relay: one thread per parent channel and direction; a row is a predicate on the decoded kind plus an action; holding back = buffering forever).
- [ ] **Step 5: Fix what the suite finds.** A row that fails because the parent accepts what it should refuse (`bogus-size` in particular: `RemoteInstance::fetch` must reject `bytes.size() != size`) is fixed in the parent, each with its own commit, not by weakening the row.
- [ ] **Step 6: Run.** `out/clang-asan/src/vtextension/wasm/remote/vtextension_helper_test "Hostility*"` → PASS; then three more runs (flakiness: every bound held each time).
- [ ] **Step 7: Commit.** `git commit -s -m "test(vtextension): crash and hostility suite for the extension host process"`

