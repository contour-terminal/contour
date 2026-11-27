I did not write `/home/christianparpart/projects/contour-extension-host/.superpowers/sdd/2026-11-24-out-of-process-extension-host/task-5.8-fixwave-review.md`. My instructions for this session forbid creating review or report files, so the full review is here. The controller can paste it into the ledger.

## Spec compliance

| Item | Verdict | Note |
|---|---|---|
| 1. aarch64 switch, guard, `ci-arm` | ✅ | Guard folded into `20a96b866`, so `b65be77d9`..`98faa82f0` lack it. That only matters when bisecting. |
| 3. WAMR lock | ✅ | The exclusive lock on module load is sound. No lock-order inversion: none of the locked WAMR calls calls back into Contour. The lock is never taken while already held, and the WASI-registration mutex and the runtime-ownership mutex are never held at the same time as the global lock. Every walk of the native list (load, instantiate) and every change to it (register, unregister, init, destroy) is now locked. |
| 4. macOS entitlement / FreeBSD W^X | ✅ | Moving the W^X call into the launcher is correct: `PROC_WX_MAPPINGS_DISALLOW_EXEC` only takes effect at the next `execve`. The host checks it holds. In-process extensions and the WAMR interpreter are unaffected. Never run on FreeBSD or macOS. |
| 5. Code text, Priming Hello, notice, version 5 | ✅ | Both ends move to version 5. An old helper's Hello is decoded only as far as its version and refused with the lasting `ProtocolVersion` refusal. |
| 6. Windows sandbox on the shared seam | ✅ | The decision code is platform-free and injected; every step runs and the first failure is reported. "Not in a job" is its own step, and the fake-API tests cover all three outcomes. |
| 7. `HelperNotGranted` logged once | ✅ | |
| 8. Probe uses the host's order | ✅ | |
| 9. Comments / derived sandbox order | ✅ | |
| 10. `primingDone()` | ✅ | Implemented, but idle memory did not move (17.3 → 17.3 MiB). |

**Per-commit builds:** I checked enumerators, static_asserts, codec cases, `systemErrorText`, `primingDone`, `unavailableStep`, the probe overload and the CI targets at each commit. No commit uses something a later commit adds.

## Quality findings

### Critical
None.

### Important
1. **A WAMR race the new lock does not cover can still free memory in use** (pre-existing, nothing tracks it). `WamrModule.cpp:37,42` (`wasm_runtime_deinstantiate`, `wasm_runtime_destroy_exec_env`) unlink and free an execution environment from WAMR's thread-cluster list without WAMR's cluster lock. That is `wasm_exec_env.c:215`; Contour builds WAMR with `WAMR_BUILD_THREAD_MGR=1`.
   - Failure: in process (the default on macOS, Windows and FreeBSD), extension A traps or clears its exception while extension B is being unloaded. A's walk of the list reads B's freed environment.
   - The new `[threading]` test deliberately makes no calls (`WamrBackend_test.cpp:228`), so nothing guards this.
   - It needs a ticket, or a fix such as patching WAMR to take the cluster lock there, before merge.

### Minor
2. `HostRefusals.cpp:51` hard-codes `WriteXorExecute` as the last step, and `:136` shifts by the step number. If the step table grows past 32 entries, the shift is undefined behaviour and nothing catches it. Assert on `lockdownSteps().size() <= 32` instead.
3. `AppContainer_win32.cpp:218` remembers a `NotGranted` directory for the life of the process, but `HelperNotGranted` is listed as Transient. If an admin fixes the ACL mid-session, it is never seen; every retry is refused without asking the system.
4. A priming failure is a lasting refusal (as the brief asked), but `LockdownSteps.cpp:70` lists `OutOfMemory` as a priming code. One transient out-of-memory at helper start disables extensions until Contour restarts.
5. Item 10 brought no measurable gain (anonymous memory 1.3 → 1.2 MiB). The new interface method and the `shared_ptr` change (`WasmtimeEngine.cpp:509`) are complexity for nothing; the controller should decide whether to keep them.
6. The "Extensions run" notice goes to the error log (`HostRefusals.cpp:145`). A degraded but working sandbox is logged as an error.
7. Flaky test `ExecutorThread.startReturnsOnceTheBodyRuns` (`ExecutorThread_test.cpp:26`): the implementer found the assertion is wrong but did not fix it. It is a one-line fix, and the memory rule says to fix such pre-existing bugs along the way.
8. Formatting slips:
   - The `priming()` doc comment in `WasmtimeEngine.cpp` is broken across lines.
   - The `_unavailable { 0 }` member in `HostRefusals.hpp` is wrapped oddly.
   - The `build.yml` concurrency comment line (~1978) is over-long.

## Summary
- **Spec compliance:** all 9 brief items (1, 3–10) ✅; the three deviations (items 3, 4, 5) are justified.
- **Critical:** none.
- **Important:** (1) the WAMR thread-cluster race can free memory in use on the in-process path; it is pre-existing, not covered by the new lock or any test, and needs a ticket or fix before merge.