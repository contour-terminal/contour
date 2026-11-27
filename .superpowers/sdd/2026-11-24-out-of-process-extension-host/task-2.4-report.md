# Task 2.4 report — contract changes at the seam

Status: DONE. Commit fa61f604c `feat(vtextension): name a module's host at the runtime seam, and report a crashed one` (on a827bcbbe).

## Changes
- `wasm/WasmRuntime.hpp`: `ModuleIdentity { std::string key; operator== }`; `CallFailure::HostCrashed`; `IWasmRuntime::load(ModuleIdentity identity, std::vector<std::byte> bytes)` (doc names `HostUnavailable`).
- `ExtensionError.hpp`: `HostUnavailable` appended, text row "its host process is unavailable", static_assert re-anchored.
- `CompiledExtension.{hpp,cpp}`: `load(Engine&, wasm::ModuleIdentity, bytes)`; passes only `HostUnavailable` through, everything else stays `InvalidComponent`.
- `Instance.cpp` `guestFailure`: `if` replaced by an exhaustive `switch` (so -Wswitch names it on the next enumerator); `HostCrashed` → `HostUnavailable` (D21). Ranking unchanged: host-call error, then deadline, then the failure.
- `ExtensionHost.cpp` `prepareExtensions`: `wasm::ModuleIdentity { spec.hostId }`.
- WAMR (`WamrRuntime.hpp`, `WamrBackend.cpp`) and Wasmtime (`WasmtimeRuntime.hpp`, `WasmtimeBackend.cpp`): take and ignore the identity.
- `wasm/remote/proto/Codec.cpp`: `isEnumerator(CallFailure)` accepts `HostCrashed` (ExtensionError's check is table-sized, so it follows automatically).
- `testing/FakeWasmBackend.hpp`: `loads()`, `lastIdentity()` (mutex-guarded), plus public `std::optional<ExtensionError> loadFailure` knob (not named by the brief; needed to make the fake `load` fail, styled like the existing public `guest`). `FakeWasmRuntime::load` moved out of line, after the backend is complete.
- Callers passing `ModuleIdentity {}`: `wasm/testing/BackendContract.hpp`, `wasm/testing/RenderMeasurement.hpp`, `testing/Harness.hpp`, `testing/RunnerHarness.hpp`, `Instance_test.cpp`, `ControlImports_test.cpp`, `Engine_test.cpp`, `wasm/wasmtime/WasmtimeBackend_test.cpp`.

## Tests
- New: `CompiledExtension.passesHostUnavailableThrough` (also checks another load error still maps to InvalidComponent), `CompiledExtension.passesIdentityToTheRuntime` (also `loads() == 1`), `Instance.hostCrashedIsHostUnavailable` (Instance_test.cpp); `ExtensionError.everyErrorHasText` in new `ExtensionError_test.cpp` (added to CMakeLists); Codec_test round-trip samples gained `CallResult{HostCrashed}` and `HostResult{HostUnavailable}`, BadEnum boundary moved to `HostUnavailable`.
- Codec round-trip sections are now named by sample index too: with two samples of one PduKind, Catch2 would run only the first same-named DYNAMIC_SECTION.
- RED: `cmake --build … --target vtextension_test` failed to compile (no `HostUnavailable`, `HostCrashed`, `ModuleIdentity`, `loads`, `lastIdentity`, `loadFailure`).
- GREEN: vtextension_test 352 cases / 12017 assertions pass; vtextension_wasmtime_test 51 / 1004 pass; `ctest --preset=clang-asan -R 'vtextension|check_wasm_runtime_containment'` 4/4 pass (check_wasm_runtime_containment, its selftest, vtextension_test, vtextension_wasmtime_test).
- Build warning-free (-Werror) for both targets.

## Lint
- `clang-tidy -p out/clang-asan --quiet <changed .cpp + ExtensionError_test.cpp>`: no diagnostics.
- `/usr/bin/clang-format -i` on changed C++; `editorconfig-checker` on changed files: clean.

## Concerns
- `vtextension_remote_fuzz` is not a target in this configuration (fuzz off), so Codec.cpp was compiled only via vtextension_test.
- `guestFailure` keeps DeadlineExceeded above HostCrashed: if a later remote backend implements interrupt by killing the helper, an overrun still reports as an overrun. Revisit if 2.6+ wants the opposite.
