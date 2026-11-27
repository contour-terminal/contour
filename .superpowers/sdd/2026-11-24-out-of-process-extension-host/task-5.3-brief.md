### Task 5.3: `WamrEngine : IHostedEngine`, and a helper that runs it
Order change: this comes before the default switch, because a Linux build without Wasmtime (the Fedora RPM) must still have a helper when the default flips (D85).

**Files:**
- Create:
  - `wamr/{WamrEngine.hpp,WamrEngine.cpp,HelperEngine.cpp,WamrEngine_test.cpp}`. The test goes in `vtextension_test`, which already links WAMR.
  - `remote/testing/WamrHelperDefaultBackend.cpp` and the target `vtextension_wamr_helper_test` (D99).
- Modify:
  - `wamr/{WamrBackend.hpp,WamrBackend.cpp,WamrRuntime.hpp,CMakeLists.txt}`: the null device is injected.
  - `src/contour-extension-host/CMakeLists.txt`: the engine follows `CONTOUR_WASM_RUNTIME`; test-only `contour-extension-host-wamr`.
  - `remote/CMakeLists.txt`.
  - `cmake/ContourThirdParties.cmake`: only if the spike adopts a hook.

**Produces:**
- `wamr::NullDevice` (an owned descriptor, or `OpenPerRuntime`).
- `wamr::WamrConfig{NullDevice nullDevice}` and `makeWamrBackend(WamrConfig = {})`. In-process behaviour is unchanged.
- `wamr::makeWamrEngine() -> unique_ptr<host::IHostedEngine>`: `memoryMode() == Copy` (or `Shared` per the spike), `memoryGuardBytes() == 0`, and `prepareThread()` = WAMR's thread environment.
- The target `vtextension_wamr_helper_engine` defines `makeHelperEngine()` and `makeCopyingHelperEngine()` for WAMR.

- [ ] **Step 1: Spike** (D98): can WAMR 2.4.5 place a linear memory at a base we give and grow it in place?
  - Read `core/iwasm/common/wasm_memory.c` and the runtime's init options in the CPM checkout (`out/clang-asan/_deps/wamr-src`).
  - Rule: Shared only if such a hook exists **and** the configure probe can detect it in a system WAMR. Otherwise Copy.
  - Record the finding as a Phase-5 fact. Expected: Copy.
- [ ] **Step 2: Write the engine tests.**
  - `"WamrEngine.copiesMemory"`: `memoryMode() == Copy`, `memoryGuardBytes() == 0`.
  - `"WamrEngine.runtimesShareOneNullDevice"`: three runtimes and the engine's open-descriptor count stay unchanged. No `open()` happens after construction (counted through an injected opener).
  - `"WamrEngine.prepareThreadIsIdempotent"`.
  - The contract runs over a `RemoteBackend` in-process `HostServer` with `makeWamrEngine()` (a `[wasm-contract]` composition in remote_test, beside the Wasmtime one).
- [ ] **Step 3: Write the WAMR helper suite.** `vtextension_wamr_helper_test` is the contract, `[bundled]` and the Review Focus process cases against `contour-extension-host-wamr`, with `RequireLockedDown` on Linux. `"Helper.wamrHelperSaysCopy"`: Hello `{Copy, LockedDown}`.
- [ ] **Step 4: Build** `vtextension_test vtextension_remote_test contour-extension-host-wamr vtextension_wamr_helper_test`; expect failure.
- [ ] **Step 5: Implement** `NullDevice` injection, `WamrEngine` and `HelperEngine.cpp`. The engine opens the null device at construction, which is before lockdown (D98).
- [ ] **Step 6: Wire the helper's engine** (D99): `contour-extension-host` links `vtextension_${CONTOUR_WASM_RUNTIME}_helper_engine`. With Wasmtime present and the runtime `wasmtime`, `contour-extension-host-wamr` is also built (never installed). The status line `contour-extension-host is not built: …` goes.
- [ ] **Step 7: Run** `vtextension_test "WamrEngine.*" "[wasm-contract]"`, `remote_test "[wasm-contract]"` (both engines) and `vtextension_wamr_helper_test` whole → PASS; then the WAMR helper suite in the release tree; ctest → PASS.
- [ ] **Step 8: Commit** (two):
  - `feat(vtextension): the extension host can run its modules interpreted`
  - `build: build the extension host with whichever engine the build selected`


---

## Item 0: carried from the review of the Linux sandbox (do these first, as their own commit(s), each building alone)

1. **A real crash after lockdown must not look like a sandbox kill.** Add allow rows for `writev` (glibc's `__libc_message` uses it on heap corruption) and `rt_sigaction` (Wasmtime hands a non-guest fault on by restoring `SIG_DFL` through `sigaction`). Neither grants anything beyond `write`/`sigaltstack`. Replace the test that pins `rt_sigaction` as killed (`SeccompFilter_test.cpp` ~line 129) with one that pins it allowed, and give each row its reason. Both architectures in `systemCallNumbers()`.
2. **`assembleFilter`'s `unlisted` overload** (`SeccompFilter.cpp:166`, documented at `SeccompFilter.hpp:42-55`): a row whose argument checks fail is killed even in an allow-by-default filter. Route failed checks to the unlisted action in that overload, and document it. The probe's outer filters must keep their current behaviour for the calls they target (adjust their rows if needed). Add an evaluator test.
3. **`SystemCallRows.cpp:183`**: the comment claims the empty path is checked; seccomp cannot read the path. Say instead that a path stat remains possible, and that this is accepted residual risk (stat metadata stays observable).
4. **`Sandbox_linux.cpp:45-47`**: the sanitizer build's Landlock rule covers all of `/proc`. Open `/proc/self` with `O_PATH` instead (it resolves to `/proc/<pid>` at open time), so the rule covers only the helper itself. Confirm that the sanitizer helper_test still passes sandboxed.
5. **`src/vtextension/wasm/host/SanitizerOptions.cpp`**: its `__asm__` labels lack Mach-O's underscore prefix, so on macOS the hook does nothing. Compile the file into Linux builds only (it serves the Linux filter), and say so in its comment.

## Controller notes (decisions that bind this task, from the plan)

- **D98 (deviation, accepted as R41):** the WAMR helper keeps WAMR's own WASI (`LIBC_WASI`), not our shim. One null device is opened when the engine is made, before lockdown, and every runtime shares it. **The spike is time-boxed to one hour.** If you find no in-place linear-memory hook that a *system* WAMR also exposes, Copy is final for this branch. Record the finding, with the file and function names you read, in the report under "Phase-5 facts".
- **D99:** the helper links the engine that `CONTOUR_WASM_RUNTIME` names. Where Wasmtime is also present, the test-only `contour-extension-host-wamr` and `vtextension_wamr_helper_test` are built and never installed.
- **D93 applies to the WAMR helper:** measure its system-call set with strace in the trees you run (release at least, ASan too). If WAMR needs calls that the Wasmtime helper's rows lack, add them as rows with reasons. Do not widen W^X: an interpreter needs no executable mapping at all, so if the row tables can express that, the WAMR helper's set should drop `PROT_EXEC` from `mprotect` as well, through a row set chosen by the engine or build. Otherwise say why not.
- **The WAMR helper must report `LockedDown`** (Hello `{Copy, LockedDown}`) under `RequireLockedDown` on this Linux box.
- **Each commit must build on its own.** Commit messages never mention the plan, task numbers or "phase". `git commit -s`, no AI trailers. User-facing text never says wasm, WAMR or Wasmtime.
