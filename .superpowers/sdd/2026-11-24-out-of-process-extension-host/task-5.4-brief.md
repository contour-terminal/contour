### Task 5.4: The default composition becomes out-of-process on Linux
**Files:**
- Create: `remote/process/{HostComposition.hpp,HostComposition.cpp,HostComposition_test.cpp}` (process_test), `remote/DefaultBackend.cpp` (target `vtextension_out_of_process`).
- Modify:
  - `CMakeLists.txt` (top level): `CONTOUR_EXTENSION_HOST`, `_CONTOUR_SANDBOXED_PLATFORMS`, `CONTOUR_WASM_RUNTIME` defaulting, summary lines.
  - `src/vtextension/CMakeLists.txt`: `vtextension_runtime`; `vtextension_test` links `vtextension_wamr` by name.
  - `src/contour-extension-host/CMakeLists.txt`: `contour_place_extension_host_beside()`.
  - `src/contour/CMakeLists.txt`, `src/vtbackend/CMakeLists.txt` (bench-headless).
  - `process/ProcessLauncher.hpp` and `Process_*.cpp`: the sanitizer environment row from 5.2 applies to production compositions too.
  - `scripts/check-wasm-runtime-containment.py`: only if the new target trips a rule; the new rules are Task 6.2's.

**Produces:**
- `process::HostComposition` (supervisor over `makeSystemProcessLauncher()`, kind, `ProcessHostConnector`).
- `process::sharedHostComposition(std::filesystem::path helper) -> std::shared_ptr<HostComposition>` (weak-cached, D101).
- `makeDefaultBackend(BackendConfig)` for out-of-process. With a null connector it locates the helper with `locateExtensionHost(vtpty::currentExecutablePath())`. If not found → every load `HostUnavailable`, with the `HelperNotFound` line logged once.
- CMake `contour_place_extension_host_beside(<target>)`.

- [ ] **Step 1: Write the tests.**
  - `"HostComposition.backendsShareOneSupervisor"`: two backends, one supervisor. Dropping both stops it, and a third backend gets a new one.
  - `"HostComposition.missingHelperRefusesEveryLoad"`: `HostUnavailable`, no launch, one log line.
  - `"HostComposition.teardownGraceFollowsTheBuild"`.
- [ ] **Step 2: Write the configure checks** (a script in the scratchpad, recorded in the report). Fresh configures under `<scratchpad>/cfg-*`:
  - `-DCONTOUR_EXTENSION_HOST=in-process -DCONTOUR_WASM_RUNTIME=wasmtime` → FATAL_ERROR naming both.
  - The default on Linux → `out-of-process` with `wasmtime` (CPM).
  - `-DCONTOUR_USE_CPM=OFF` with no Wasmtime → `out-of-process`, `wamr`.
  - An unknown value → FATAL_ERROR.
- [ ] **Step 3: Implement CMake** (D100).
- [ ] **Step 4: Implement the composition and the default backend** (D101, D102).
  - `ContourGuiApp`, `ContourApp` and `bench-headless` stay unchanged, as they call `makeDefaultBackend(BackendConfig{})`.
  - The build tree gets the helper copied beside `contour` and `bench-headless`. On macOS, the bundle's `Helpers/` copy stays as it is.
- [ ] **Step 5: Prove the GUI and daemon link no engine.**
  - `nm -C out/clang-asan/src/contour/contour | grep -cE 'wasm_runtime_|wasmtime_'` → 0.
  - The same for `bench-headless`.
  - `contour-extension-host` has them.
- [ ] **Step 6: Run the suites on the new default.**
  - ctest `-R 'vtextension|wasm_runtime_containment'` → PASS.
  - `bench-headless` extension rows run through a helper: the supervisor launch count > 0 in its output, or `strace -f -e trace=execve` shows the helper.
  - The `e2e`-labelled app runs pass (`ctest --preset=clang-asan -L e2e`).
- [ ] **Step 7: Run a manual check from the build tree** (offscreen, PID-recorded, killed by PID): `QT_QPA_PLATFORM=offscreen` contour with the bundled status-line extension renders. `pgrep -P <contour pid>` shows `contour-extension-host`, whose `/proc/<pid>/status` says `Seccomp: 2`, `NoNewPrivs: 1`.
- [ ] **Step 8: Commit** (two):
  - `feat(vtextension): run extensions in a sandboxed extension host process by default on Linux`
  - `build: select where extensions run and which engine the extension host uses`


---

## Plan decisions this task implements (verbatim)

- D100. CMake:
  - `CONTOUR_EXTENSION_HOST` takes `out-of-process` or `in-process`. The default is `out-of-process` iff `CMAKE_SYSTEM_NAME` is in `_CONTOUR_SANDBOXED_PLATFORMS`; Linux is added in 5.4, and the others by their tasks.
  - `CONTOUR_WASM_RUNTIME` now names the engine: default `wasmtime` if one resolved and out-of-process, else `wamr`. `in-process` + `wasmtime` → FATAL_ERROR.
  - Only programs link `vtextension_runtime`; test binaries name their composition (`vtextension_test` links `vtextension_wamr`).

  — One table row per platform flip, and tests do not silently start spawning helpers.
- D101. `process::HostComposition` (system launcher, supervisor, kind, connector) is shared by every default backend in a process through a weak-cached `sharedHostComposition()`. The supervisor ends with the last backend, not at static destruction. Teardown grace follows `ThisBuildInstrumentation`. — One supervisor per hosting process (spec §5.3), with no `jthread` joined during exit.
- D102. In the build tree, a POST_BUILD copy places the helper beside `contour` and `bench-headless`, and `locateExtensionHost`'s "beside" candidate finds it. No build-tree path is compiled into a binary. — Developer runs work, and installed binaries never consult a path outside their install.

## Item 0: carried over from earlier reviews (do these first, each in its own commit, and each commit must build on its own)

1. **Refusals that should stay refused.** Today a refusal stays sticky for a connector only when it is `LockdownFailed`. Once this task makes `RequireLockedDown` the default, two more refusals describe a property of the system or the build, and retrying them cannot succeed: `Unsandboxed` under `RequireLockedDown`, and `ProtocolVersion`. Make them sticky as well. To make that possible, add a data-driven classification (a table) on `HostRefusal`, and make `IHostConnector::refused()` consult it. Add a test for each refusal kind.
2. **The probe always uses `GuestCode::Compiled`.** Give the sandbox probe a `GuestCode` parameter. Then add one case that proves, on the real kernel, that under the interpreted row set `mprotect(PROT_EXEC)` is killed (and `mmap` with exec too), while under the compiled set it is allowed RX.
3. **Small cleanups:**
   - `SeccompFilter_test.cpp:49-51`: the Doxygen comment wraps badly.
   - `cmake/ContourThirdParties.cmake:182-194`: the probe comment does not mention "no JIT", and lines 217-220 are badly rewrapped.
   - `HelperEngine.cpp:24`: the message reads awkwardly ("no engine: its engine was built…"). Reword it, and keep it free of wasm, WAMR and Wasmtime.
   - `WasiOverrides.cpp:115`: check the result of `findShimFunction()` before dereferencing it. Fail loudly at registration rather than crash.

## Controller notes (decisions that bind this task)

- **The engine selection must be folded.** Task 5.3 introduced `CONTOUR_EXTENSION_HOST_ENGINE` in `src/CMakeLists.txt` as a stopgap: `wasmtime` where Wasmtime was found, otherwise `CONTOUR_WASM_RUNTIME`. That stopgap was needed because `CONTOUR_WASM_RUNTIME` still accepted only `wamr`. This task implements D100's `CONTOUR_WASM_RUNTIME` defaulting, so remove the stopgap. There must be one variable, a packager must be able to choose it, and it must appear in the configure summary. Also fix the stale Wasmtime status text ("only its own tests use it so far").
- **The brief's "sanitizer environment row from 5.2" no longer exists.** A fix round in 5.2 replaced it with compile-time `__asan_default_options` / `__lsan_default_options` (`host/SanitizerOptions.cpp`, Linux only). Production compositions therefore need no environment row. Verify that a sanitizer-built `contour` starts helpers that exit cleanly, and do not re-add an environment row.
- **R43: only Linux flips** to out-of-process by default. macOS, FreeBSD and Windows keep in-process, because their sandboxes have not landed yet. They land, dormant, in the next three tasks.
- **Draft PR #2431 is live.** Every commit must pass these checks, so run them before you report:
  - `/usr/bin/clang-format --dry-run --Werror` on the changed C++ files;
  - `typos` (v1.48.0 is installed for the CI fixes; the report `task-ci1-report.md` says where);
  - `editorconfig-checker`.
  
  For portability, read `task-ci1-report.md` for the traps CI found on macOS (unused variables under Apple clang), on libstdc++ 14 (`ranges::contains` against `nullptr` on smart pointers), and on MSVC (`Sandbox.hpp` needs `vtextension_host` linked).
- **Do not push.** The controller pushes.
- **Commit messages** never mention the plan, task numbers or "phase". Use `git commit -s`, with no AI trailers. User-facing text never says wasm, WAMR or Wasmtime.
