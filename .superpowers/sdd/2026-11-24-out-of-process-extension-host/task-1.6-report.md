# Task 1.6 report: sticky interruption by epoch, no ticker (plus the 1.4/1.5 carry-ins)

Status: DONE. Branch `feature/out-of-process-extension-host`, not pushed. Base c9b6169d1.

## Commits

| SHA | Subject | Item |
|---|---|---|
| 6f2ad62a0 | fix(vtextension): a refusing random source fails random_get, not the process | 0.f |
| 7543d4865 | test(vtextension): keep the WASI module's WebAssembly text readable | 0.e |
| de7e324e4 | fix(vtextension): a WAMR instance runs again after one of its calls failed | found while doing 0.a |
| f9ac285df | fix(vtextension): a nested call's host failure stays with the nested call | 0.a |
| 3e8212fc3 | fix(vtextension): an unexported memory over the cap is out of memory under Wasmtime | 0.b |
| 97e7f0108 | refactor(vtextension): Wasmtime calls allocate nothing and re-enter through the caller | 0.c + 0.d |
| db0658c58 | feat(vtextension): interrupt Wasmtime calls by epoch, never polling while idle | 1.6 |

## Item 0 (carry-ins)

**a. Nested host failure leaking into the outer call** (f9ac285df)
- Wasmtime (`WasmtimeInstance::call`): the failure is computed first, then, when the call returning is nested
  (depth > 0 after the decrement), `StoreData::hostFailure` is reset. A nested call reports its failure
  through its own return value; the host function decides.
- WAMR (`WamrInstance::call` -> `endNestedCall(NestedCallOutcome)`): resets `_fault` on every nested return.
  On a failed nested call it also clears WAMR's exception. WAMR keeps a failed call's exception on the
  module instance, and its interpreter checks it right after the native call returns, so without the
  clear the outer call died the moment the host function returned. That held even when the guest would
  have gone on to return normally. An interrupt's exception is set again if it landed before the clear
  (flag re-check, then `wasm_runtime_terminate`).
- Contract: `NestedCallModule` (exports `fail`, `absorb`, `absorb-then-trap`) and `AbsorbingHostContext`
  (`ping(0)` calls `fail` back and absorbs its HostFailure; `ping(1)` fails), with the case
  `WasmContract.nestedHostFailureStaysNested`. The case requires:
  - the nested call reports HostFailure;
  - `absorb-then-trap` reports **Trap**;
  - `absorb` then **returns 7**.
- RED on both engines before the fix: Wasmtime `3 == 0` (HostFailure, not Trap); WAMR the same, and in
  addition `absorb` failed.

**Extra, a pre-existing WAMR bug found here** (de7e324e4): WAMR never cleared an instance's exception
between top-level calls, so after one trap *every* later call into that instance failed. Wasmtime runs on,
as WebAssembly allows. The outermost call now clears the exception (`wasm_runtime_clear_exception`)
*before* checking the interrupt flag, so an interrupt landing in between is still seen. The contract case
`WasmContract.callAfterTrapRuns` (grow traps under a 2-page cap, then `add(2,3)` must give 5) was RED on
WAMR and GREEN on Wasmtime. It was committed on its own, before (a), because (a)'s "absorb after trap"
assertion depends on it.

**b. Unexported memory above the cap -> OutOfMemory** (3e8212fc3)
- New `binary::largestInitialMemoryPages(module)` in `WasmBinary.hpp`, next to the existing section
  reader. It returns the largest initial page count over the memory section, or 0 if there is none.
  - Accepted limits flags: 0..3 (has-max, shared).
  - Any other flag (memory64, custom page size) gives `Malformed`. No engine of the host accepts those, so
    `BinaryError` and WAMR's `ModuleBytesError` mapping need no new enumerator.
- `loadModule` computes it from the bytes. `WasmtimeModule` stores `_largestInitialPages`, and
  `WasmtimeInstance::create` checks `pages * 64 KiB > cap` before creating the store. This **replaces** the
  exports-only check.
- Tests:
  - table test `WasmBinary.largestInitialMemoryPages`, 9 rows plus not-a-module;
  - contract `WasmContract.memoryCapRefusesUnexportedInitialSize`, with `UnexportedMemoryModule` =
    `(module (memory 1))`. It was RED on Wasmtime (InvalidComponent) and already GREEN on WAMR.

**c. No per-call vectors** (97e7f0108): `valuesFor(count, std::array<wasmtime_val_t, 16>&, std::vector&)`.
Arguments and results each use a fixed 16-wide array, the Canonical ABI's flat-parameter limit. Only a
wider export resizes the vector. The new `WasmtimeBackend.callsWideExports` calls a 20-parameter export
with 1..20 and expects the sum of squares (2870). The rearrangement inequality makes that unique to the
right order, so the vector path is covered too. This is a coverage test, not RED (the old code passed it
too).

**d. Caller context for nested calls** (97e7f0108): `StoreData::callerContext` (`wasmtime_context*`,
forward-declared so the header stays free of wasmtime.h). `callHostFunction` sets it with `std::exchange`
for the duration of `callHost` and then restores the outer value, which makes nested host calls correct.
`WasmtimeInstance::storeContext()` returns it while it is set, otherwise `wasmtime_store_context`. Both
`call()` and `memory()` use it. No test can observe the difference (it is the same store); it is covered
by `hostFunctionCallsBackIn` and `nestedHostFailureStaysNested` exercising the path.

**e.** The WAT comment for `EveryWasiFunctionModule` is restored, one form per line, inside
`// clang-format off` ... `// clang-format on`. The guards wrap the whole doc comment and the array, so the
Doxygen block stays contiguous. The script that rewrote it asserted token-identity with the reflowed text.

**f.** (6f2ad62a0)
- `ShimEnvironment::random` is now `std::function<std::expected<void, Errno>(std::span<std::byte>)>`.
- `Errno::Io = 29` was added per witx. `Errno` moved above `ShimEnvironment`.
- `randomGet` chains with `and_then`. `SystemEnvironment::fillRandom` returns `Io` where it used to call
  `std::terminate` (getrandom non-EINTR failure, BCryptGenRandom failure).
- The Wasmtime binding needed no change: it forwards the shim's errno.
- Tests: the new `Preview1Shim.random_getFailsWithTheSource` checks Io, untouched memory, and that other
  functions still work. `Guest` gained an environment constructor, and the existing users were updated.

## Task 1.6

- `enum class InterruptState : uint8_t { Running, Interrupted }` is in `WasmtimeRuntime.hpp`.
  `std::atomic<InterruptState> interrupt` is in **StoreData**, not a member of the instance; see Deviations.
- **Deadline:** `DeadlineTicks = 1`, and `armEpochDeadline` sets `current_epoch + 1` before every guest
  entry and before instantiation (start functions). The max-delta placeholder is gone, so there is nothing
  left to wrap.
- **Callback** `onEpochDeadline`: registered with `wasmtime_store_epoch_deadline_callback(store,
  &onEpochDeadline, data, nullptr)` right after the store is created, before instantiation. It:
  1. counts `KindState::countEpochCallback()`;
  2. takes an acquire fence;
  3. reads its own instance's flag. `Interrupted` returns `wasmtime_error_new("interrupted")`. Otherwise it
     sets `*delta = 1` and `CONTINUE`, and returns null.
- **`interrupt()`:** `interrupt.store(Interrupted, relaxed)`, then a release fence, then
  `wasmtime_engine_increment_epoch(_engine)`. No lock and no allocation, so it is wait-free: the increment
  is a lock-free fetch_add, documented async-signal-safe. The instance keeps `wasm_engine_t const* _engine`,
  kept alive through owner -> module -> runtime.
- **`failure()`:** flag, then hostFailure, then Trap. The old `WASMTIME_TRAP_CODE_INTERRUPT` branch is gone:
  with a callback always installed, an interrupt ends a call only through the callback, which reads the
  same flag.
- **Test hook:** an atomic `_epochCallbacks` on `KindState`, with `countEpochCallback()` (production calls
  it in the callback, which only runs after an interrupt) and a test-only reader `epochCallbacks()`. There
  is no global state, and it is not part of the public `WasmtimeBackend.hpp` (KindState stays opaque there).
  The test includes the internal `WasmtimeRuntime.hpp`.
- **Engine config comment** updated: no ticker; only `interrupt()` moves the epoch.

### The race between call()'s check and guest entry

`call()` **sets the deadline first, then takes an acquire fence, then checks the flag.** The brief's
literal order is check-then-arm; see Deviations. There are three cases for an `interrupt()` racing it.
`interrupt()` stores the flag, fences, then increments.

1. **It lands before the deadline is set.** Setting the deadline reads the *incremented* epoch. The fences
   pair (release before the RMW, acquire after the read), so the flag check sees `Interrupted` and the call
   returns Interrupted without entering the guest.
2. **It lands between setting the deadline and the check.** If the check already sees the flag, the call
   returns at once. If not, the deadline is `E+1` against an epoch now `E+1`, so the guest's first epoch
   check (function entry) calls the callback. The callback's fence pairs with interrupt()'s fence; it sees
   the flag and traps, and the call reports Interrupted.
3. **It lands after the check.** Same as 2: the deadline was set before entry and the bump reaches it.

With the brief's order (check, then arm), an interrupt landing between the two would set the deadline
relative to the already-bumped epoch, and the guest would spin forever. That is the hazard the
coordinator flagged.

**Test** `Wasmtime.interruptRacingACallStopsIt`, 200 rounds, a fresh instance each:
- An interrupter thread waits on a `go` flag, spins `round*4` atomic increments, then interrupts.
- The main thread releases `go` and calls `spin` at once.
- Every round must end Interrupted. A wrong order hangs, and the ctest timeout fails it.

This is **not deterministic** for the real window, which is sub-microsecond. Mutation evidence:
- With the check moved before the arm and a `yield()` in between, 200/200 rounds still passed: the window
  was never hit.
- With a 1 ms sleep in that window, the test **hung** (timeout 124).

So the test catches a wrong order whenever the window is hit; the correctness argument above is what
covers the real window. A deterministic test would need a seam inside `call()` between the arm and the
entry, which I judged not worth a production hook.

## TDD evidence

| Case | RED (before) | GREEN |
|---|---|---|
| nestedHostFailureStaysNested | Wasmtime 1 failed (`3 == 0`), WAMR 2 failed | both pass |
| callAfterTrapRuns | WAMR failed (`add` after trap) | pass |
| memoryCapRefusesUnexportedInitialSize | Wasmtime failed (InvalidComponent), WAMR passed | pass |
| interruptStopsLoop | hang (`timeout 30` gives rc 124) | pass, 8 assertions |
| interruptRacingACallStopsIt | hang (rc 124) | pass, 404 assertions |
| interruptLeavesSiblingsRunning | failed (`0 == 1` callbacks) | pass |
| idleInstanceNeverWakes | passed trivially: no callback existed, so 0 = 0. It is a guard, not a RED | pass |
| interruptIsFinal | passed before, as in 1.4 | pass |

More mutation checks:
- A callback that traps regardless of its own flag makes `interruptLeavesSiblingsRunning` fail at the
  sibling's `ping-then-count`.
- That test also pins exactly one wake after one foreign interrupt. A max-delta or wrapping deadline would
  give more wakes or a trap.

Item f's new test was written together with the type change, so its RED would have been a compile error;
it was not run separately.

## Test results (real counts)

| Run | Result |
|---|---|
| `vtextension_wasmtime_test` (full) | All tests passed (952 assertions in 43 test cases) |
| `vtextension_test` (WAMR + shared), before the 1.6 commit | All tests passed (4535 assertions in 314 test cases) |
| `ctest --preset=clang-asan -R vtextension` | 2/2 passed, no timeouts (13.5 s total) |
| TSan (`out/clang-tsan`, configured with PACE_SCHEDULER, built only `vtextension_wasmtime_test`, -j4 niced) | the interrupt cases + nested/re-entry cases: 469 assertions in 7 test cases, **0 TSan reports**. The whole binary except `WasmtimeBackend.refusesWhatWasmtimeEnablesByDefault`: **0 reports**. The full binary passes (952/43) but gives 142 reports, all from that one test. Every report is inside Wasmtime's prebuilt, uninstrumented Rust: Cranelift's parallel-compilation worker threads in the *default* engine the test builds for comparison (malloc, realloc or free racing `FunctionCompiler`/`drop_glue`). TSan cannot see that synchronisation. None has a vtextension frame on the racing access. The backend's own engines disable parallel compilation. Phase gate 1.7 should suppress the race or exclude that case under TSan. |

## Tooling

- clang-tidy, by hand, reported 0 diagnostics: `nice -n 19 clang-tidy -p out/clang-asan --quiet
  src/vtextension/wasm/wasmtime/{WasmtimeInstance,WasmtimeBackend,WasmtimeModule,WasmtimeBackend_test}.cpp
  src/vtextension/wasm/wamr/{WamrInstance,WamrBackend_test}.cpp src/vtextension/wasm/WasmBinary_test.cpp`.
  Earlier, for 0.f: `src/vtextension/wasm/wasi/{Preview1Shim,SystemEnvironment,Preview1Shim_test}.cpp`.
  The test TUs bring in `BackendContract.hpp` and `WasmBinary.hpp` through HeaderFilterRegex.
- `/usr/bin/clang-format -i` on every changed C++ file; `editorconfig-checker` clean.
- Module bytes were assembled with a scratch tool (scratchpad) linking the pinned `libwasmtime.a`:
  `wasmtime_wat2wasm` plus `wasmtime_module_validate`.

## Deviations

1. **The flag lives in `StoreData`, not as `_interrupt` on the instance.** The deadline callback's `env`
   must exist before instantiation (a start function may run, and other instances' interrupts move the
   shared epoch during it), and that is before the instance object exists. `StoreData` is now built with
   `make_unique<StoreData>(context, env, kind)` (parenthesised aggregate init), because an atomic member
   cannot be moved.
2. **Order in `call()` is arm-then-check, not check-then-arm** (see the race section). It is still checked
   before the guest is entered, so the sticky semantics are unchanged.
3. **The callback count is a counter on `KindState`** that production increments, rather than a
   test-only build. It costs one relaxed atomic increment per wake, and wakes happen only after an
   interrupt.
4. **Extra fix de7e324e4** (WAMR exception never cleared between calls) and its contract case
   `callAfterTrapRuns`. This was not in the brief; without it the honest version of the 0.a contract case
   could not pass on WAMR.
5. **0.a on WAMR also clears the nested failure's exception.** The brief said only "reset the stash".
   Resetting `_fault` alone would have made the case report Trap for the wrong reason (the leftover
   exception) and still killed `absorb`.
6. **Extra tests:** `Wasmtime.interruptLeavesSiblingsRunning`, `Wasmtime.interruptRacingACallStopsIt`,
   `WasmtimeBackend.callsWideExports`, `WasmBinary.largestInitialMemoryPages`,
   `Preview1Shim.random_getFailsWithTheSource`.

## Concerns

- **WAMR's cross-thread `wasm_runtime_terminate` against `wasm_runtime_clear_exception` is a plain write
  race** inside WAMR (pre-existing design: terminate from another thread). The flag re-check after each
  clear closes the logical window. The memory-model guarantee on WAMR's exception buffer is WAMR's.
- **The race test is statistical** for the sub-microsecond window (argued above).
- **`callerContext`** follows store.h's rule, but no test can tell the two contexts apart.

---

# Fix report (review round 1)

Commits: `9b1e098f4 fix(vtextension): an interrupt during a host function call stops the Wasmtime guest`
(items 1, 2, 3, 5) and `1d1313dc7 fix(vtextension): a nested WAMR call keeps what the outer call recorded`
(item 4).

1. **A nested Interrupted return makes the outer deadline due** (`WasmtimeInstance.cpp`).
   - The new `interrupted(context)` helper does the work. When the call is nested (`_callDepth > 0`), it
     calls `wasmtime_context_set_epoch_deadline(context, 0)` and then returns
     `unexpected(Interrupted)`.
   - Both paths use it: the early flag-check return, and a nested call whose guest was stopped by the
     callback.
   - Test `Wasmtime.nestedInterruptStopsTheOuterCall`:
     - The host function interrupts its own instance, calls `add` back in (Interrupted), absorbs that,
       and returns into a `(loop (br 0))`.
     - The call runs under `std::async` and waits at most 5 s. On timeout the test interrupts again (the
       second epoch bump reaches the pushed-out deadline and releases the thread), so a regression fails
       instead of hanging.
   - RED by mutation: with the deadline reset removed, `CHECK( ended )` FAILED (1 failed of 10
     assertions) and the test still terminated. It is GREEN with the fix.
2. **Lost-wakeup window documented, not closed (R7).**
   - At `onEpochDeadline` the comment covers:
     - what the window is: the callback reads the flag, then Wasmtime re-reads the epoch to count the new
       relative deadline, and an interrupt landing between the two goes unseen until the epoch moves
       again;
     - why it cannot be closed: the callback only returns a delta relative to Wasmtime's later read, and
       there is no absolute or compare-and-set deadline, here or in `wasmtime_context_set_epoch_deadline`;
     - when it can happen: two instances sharing an engine, plus a few-instruction window;
     - that the in-process backend serves tests, and the helper process's supervisor grace timer kills a
       call that never returns.
   - `interrupt()`'s doc points to it.
3. **TSan.** `defaultEngineAccepts` sets `wasmtime_config_parallel_compilation_set(config, false)`.
   - Rebuilt `out/clang-tsan` with `flock -o ... nice -n 19 ionice -c 3 cmake --build --preset clang-tsan
     -j 4 --target vtextension_wasmtime_test`.
   - Ran `out/clang-tsan/src/vtextension/wasm/wasmtime/vtextension_wasmtime_test`, the whole binary with
     no exclusion: rc=0, `All tests passed (969 assertions in 45 test cases)`, and
     `grep -c "WARNING: ThreadSanitizer"` gives **0**.
4. **WAMR outer fault kept** (`WamrInstance.cpp`).
   - A nested call takes `outerFault = std::exchange(_fault, nullopt)` after its interrupt check, so it
     starts its own record. `endNestedCall(outcome, outerFault)` restores the outer record.
   - New contract case `WasmContract.nestedCallKeepsTheOuterRecord`, with `GrowThenNestedCallModule`:
     grow by 4 pages and ignore the refusal (cap 2 pages), then `ping(0)` (`AbsorbingHostContext` absorbs
     the nested HostFailure), then `unreachable`.
     - Expected result: `MemoryGrowthRefused` where growth is Reported (WAMR), `Trap` where it is not
       (Wasmtime).
   - RED against the old WamrInstance.cpp (stashed): `CHECK( called.error() == expected )` FAILED. GREEN
     with the fix.
5. **`idleInstanceNeverWakes`** now calls `count` (10⁶ iterations) repeatedly for at least 100 ms of wall
   time, and requires every result to be correct and the callback count to stay unchanged.

Test results:
- `vtextension_test`: All tests passed (4558 assertions in 317 test cases).
- `vtextension_wasmtime_test` (ASan): All tests passed (969 assertions in 45 test cases).
- `ctest --preset=clang-asan -R vtextension`: 100% tests passed, 0 tests failed out of 2 (13.5 s).
- TSan: as in item 3, 0 reports.
- clang-tidy reported 0 diagnostics: `nice -n 19 clang-tidy -p out/clang-asan --quiet
  src/vtextension/wasm/wasmtime/{WasmtimeInstance,WasmtimeBackend_test}.cpp
  src/vtextension/wasm/wamr/{WamrInstance,WamrBackend_test}.cpp`.
- editorconfig-checker: clean.

This supersedes the earlier TSan row: the 142 reports are gone at the source, with nothing suppressed or
excluded.
