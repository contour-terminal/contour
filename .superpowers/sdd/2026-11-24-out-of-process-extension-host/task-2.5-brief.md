### Task 2.5: `makeDefaultBackend(BackendConfig)`

**Files:**
- Modify: `src/vtextension/wasm/DefaultBackend.hpp` (includes `wasm/BackendConfig.hpp`), `wasm/wamr/DefaultBackend.cpp` (ignores the config, D16)
- Modify composition roots: `src/contour/ContourGuiApp.cpp` (GUI), `src/contour/cli/ContourApp.cpp` (daemon resources), `src/vtbackend/bench-headless.cpp`, `src/vthost/SessionHost_test.cpp`, `src/vtextension/testing/{Harness.hpp,RunnerHarness.hpp}`, and every `wasm::makeDefaultBackend()` in `vtextension/*_test.cpp` (grep)
- Create: `src/vtextension/wasm/remote/testing/RemoteDefaultBackend.cpp` (defines `makeDefaultBackend(BackendConfig)` → `makeRemoteBackend` over an `InProcessHostConnector` + Wasmtime when `config.connector` is null, and `wasm::testing::preparedGeneration()` → `remote::preparedGeneration()`)
- Modify: `wasm/remote/CMakeLists.txt` (`vtextension_remote_test` also compiles `Bundled_test.cpp`, `Instance_test.cpp`, `ExtensionHost_test.cpp`, `Engine_test.cpp`, `ExtensionWorkerPool_test.cpp`, `ControlImports_test.cpp` and `RemoteDefaultBackend.cpp`, with `VTEXTENSION_TEST_MODULE_DIR` and the guest dependencies copied from `vtextension_test`; links `glaze::glaze` privately)

**Interfaces:**
- Consumes: `BackendConfig` (2.3).
- Produces: `[[nodiscard]] std::unique_ptr<IWasmBackend> makeDefaultBackend(BackendConfig config);` — no default argument.

- [ ] **Step 1: Change the declaration; expect compile failures at every root.** `… --target contour vtextension_test bench-headless vthost_test`.
- [ ] **Step 2: Update each root** to `vtextension::wasm::makeDefaultBackend(vtextension::wasm::BackendConfig {})`. GUI and daemon roots keep a named local `BackendConfig` so phase 3 has one place to fill the helper path and launcher.
- [ ] **Step 3: Add the remote composition to `vtextension_remote_test`.** Cases depending on in-process internals that fail there are tagged `[in-process]` and excluded on that binary with `~[in-process]`, each with a one-line reason in "Phase-2 facts".
- [ ] **Step 4: Run.** `ctest --preset=clang-asan -R 'vtextension|bench_headless'` and `out/clang-asan/src/vtextension/wasm/remote/vtextension_remote_test "~[in-process]" "~[.measure]"` → PASS (Bundled cases run real guests through `RemoteBackend`).
- [ ] **Step 5: Commit.** `git commit -s -m "refactor: configure the extension runtime at every composition root"`

