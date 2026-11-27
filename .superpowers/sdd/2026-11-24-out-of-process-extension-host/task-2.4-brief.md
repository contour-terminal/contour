### Task 2.4: Contract changes at the seam

Runs right after 2.1 (D1).

**Files:**
- Modify: `src/vtextension/wasm/WasmRuntime.hpp`, `src/vtextension/ExtensionError.hpp` (text table row), `src/vtextension/CompiledExtension.{hpp,cpp}`, `src/vtextension/Instance.cpp` (`guestFailure`), `src/vtextension/ExtensionHost.cpp` (`prepareExtensions` passes the identity), the WAMR and Wasmtime `load` implementations, `testing/FakeWasmBackend.hpp`, `wasm/testing/BackendContract.hpp`, and every `CompiledExtension::load` / `runtime.load` caller (grep)

**Interfaces:**
- Produces:
  ```cpp
  namespace vtextension::wasm {
  /// Which host a module runs in. Modules with equal keys share one; an empty key gets a host of its own.
  /// In-process backends ignore it.
  struct ModuleIdentity { std::string key; bool operator==(ModuleIdentity const&) const = default; };
  enum class CallFailure : uint8_t { Trap, Interrupted, MemoryGrowthRefused, HostFailure,
                                     HostCrashed /* the host running the instance died; every later call fails so */ };
  class IWasmRuntime { /* … */ [[nodiscard]] virtual std::expected<std::shared_ptr<IWasmModule>, ExtensionError>
      load(ModuleIdentity identity, std::vector<std::byte> bytes) = 0; };
  }
  namespace vtextension {
  enum class ExtensionError : uint8_t { /* … */ ModuleTooLarge, HostUnavailable /* appended */ };
  // CompiledExtension::load(Engine& engine, wasm::ModuleIdentity identity, std::vector<std::byte> bytes);
  }
  ```
  `HostUnavailable` text: `"its host process is unavailable"` (user-visible; names no engine). `Instance::guestFailure`: `HostCrashed` → `HostUnavailable` (D21). `FakeWasmBackend` gains `[[nodiscard]] std::size_t loads() const noexcept` and `[[nodiscard]] wasm::ModuleIdentity lastIdentity() const`.

- [ ] **Step 1: Write the failing tests.** `"CompiledExtension.passesHostUnavailableThrough"` (fake `load` returns `HostUnavailable` → `CompiledExtension::load(...).error() == HostUnavailable`; today `InvalidComponent`); `"CompiledExtension.passesIdentityToTheRuntime"` (`fake.lastIdentity() == ModuleIdentity{ "x" }`); `"Instance.hostCrashedIsHostUnavailable"` (an export returning `CallFailure::HostCrashed` → `render(...).error() == HostUnavailable`); `"ExtensionError.everyErrorHasText"` covers the new row.
- [ ] **Step 2: Build; expect compile failure.** `… --target vtextension_test vtextension_wasmtime_test`.
- [ ] **Step 3: Change the seam and its implementers.** WAMR and Wasmtime take `ModuleIdentity` and ignore it. `CompiledExtension::load` passes only `HostUnavailable` through, the rest stays `InvalidComponent`. Contract helpers pass `ModuleIdentity {}`. `prepareExtensions` passes `ModuleIdentity { spec.hostId }` until 2.6 replaces it with the registry key.
- [ ] **Step 4: Update callers mechanically.** Remaining call sites pass `wasm::ModuleIdentity {}` (tests) or the spec's identity.
- [ ] **Step 5: Run.** `out/clang-asan/src/vtextension/vtextension_test` and `out/clang-asan/src/vtextension/wasm/wasmtime/vtextension_wasmtime_test` → all PASS.
- [ ] **Step 6: Commit.** `git commit -s -m "feat(vtextension): name a module's host at the runtime seam, and report a crashed one"`

