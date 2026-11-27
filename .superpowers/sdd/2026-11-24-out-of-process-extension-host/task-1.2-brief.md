### Task 1.2: The engine feature table, enforced by both engines

**Files:**
- Create: `src/vtextension/wasm/EngineFeatures.hpp`
- Modify: `src/vtextension/wasm/testing/BackendContract.hpp` (new case + new modules, macro list)
- Modify: `cmake/ContourThirdParties.cmake` (WAMR CPM options for disabled features, if a test shows
  WAMR accepts one)

**Interfaces:**
- Produces:
  ```cpp
  namespace vtextension::wasm {
  enum class EngineFeature : uint8_t { BulkMemory, SignExtension, SaturatingFloatToInt, MutableGlobals,
      MultiValue, ReferenceTypes, Gc, Exceptions, Threads, Memory64, MultiMemory, TailCalls, Simd,
      RelaxedSimd, ComponentModel };
  enum class FeatureState : uint8_t { Off, On };
  struct FeatureRow { EngineFeature feature; FeatureState state; std::string_view name; };
  /// The one table both engines configure from and the world check reports by name.
  inline constexpr auto EngineFeatures = std::to_array<FeatureRow>({ /* states per Global Constraints */ });
  }
  ```

- [ ] **Step 1: Write the failing contract case.** In `BackendContract.hpp` add four hand-assembled
  modules, each needing one OFF feature: `SimdModule` (a `v128.const` in a function body),
  `ThreadsModule` (a shared memory), `TailCallModule` (`return_call`), `MultiMemoryModule` (two memories).
  Add:
  ```cpp
  template <typename T>
  void refusesDisabledFeatures()
  {
      auto const backend = T::make();
      auto const runtime = createRuntime(*backend);
      auto const guard = enterThread(*backend);
      for (auto const bytes : { std::span<uint8_t const>(SimdModule), std::span<uint8_t const>(ThreadsModule),
                                std::span<uint8_t const>(TailCallModule), std::span<uint8_t const>(MultiMemoryModule) })
          CHECK(runtime->load(toBytes(bytes)).error() == ExtensionError::InvalidComponent);
  }
  ```
  Register it in `CONTOUR_WASM_BACKEND_CONTRACT`.
- [ ] **Step 2: Run against WAMR.** Run:
  `out/clang-asan/src/vtextension/vtextension_test "WasmContract.refusesDisabledFeatures"`.
  Expected: it either passes, or fails naming the modules WAMR accepts.
- [ ] **Step 3: Make WAMR refuse them.** For each module WAMR accepted, add the matching CPM option
  (`WAMR_BUILD_SIMD 0`, `WAMR_BUILD_TAIL_CALL 0`, `WAMR_BUILD_SHARED_MEMORY 0`,
  `WAMR_BUILD_MULTI_MEMORY 0`). Create `EngineFeatures.hpp` with the table. A system WAMR that accepts a
  disabled feature is rejected by the existing probe path: add the check to `WamrTerminateProbe.cpp`.
- [ ] **Step 4: Run the case again.** Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -s -m "feat(vtextension): one table of WebAssembly features extensions may use"`

