### Task 2.6: `ExtensionRegistry` and the per-extension breaker

**Files:**
- Create: `src/vtextension/ExtensionRegistry.hpp`, `ExtensionRegistry.cpp`, `ExtensionRegistry_test.cpp` (vtextension_test)
- Create: `src/vtextension/wasm/remote/RegistryRemote_test.cpp` (vtextension_remote_test)
- Modify: `CompiledExtension.{hpp,cpp}` (breaker), `Instance.{hpp,cpp}` (admission + reporting), `ExtensionHost.{hpp,cpp}` (`prepareExtensions` takes the registry), `SessionExtensions.{hpp,cpp}` (`SessionExtensionServices::registry`, `ExtensionRuntime` owns one, `configurationReloaded()`), `src/contour/ContourGuiApp.cpp` (`onConfigurationReloaded`), the daemon's reload in `src/vthost/` (grep), `CMakeLists.txt` (sources)

**Interfaces:**
- Consumes: 2.4 (`ModuleIdentity`, `HostUnavailable`), 2.3/2.5 for the remote tests.
- Produces:
  ```cpp
  namespace vtextension {
  enum class ExtensionWorld : uint8_t { StatusLine };
  enum class BreakerState : uint8_t { Closed, Open };
  struct HostAdmission { uint64_t epoch = 0; };
  /// Counts an extension's host deaths, once each however many panes saw it (D20). Thread-safe.
  class HostBreaker {
  public:
      HostBreaker(core::IClock& clock, std::size_t faultsBeforeDisable, std::chrono::seconds faultWindow);
      [[nodiscard]] std::expected<HostAdmission, ExtensionError> admit() noexcept;   // HostUnavailable while Open
      void recordHostFault(HostAdmission admission) noexcept;   // counts iff admission.epoch is current; then ++epoch
      void reset() noexcept;                                    // Closed, no faults, ++epoch
      [[nodiscard]] BreakerState state() const noexcept;
      [[nodiscard]] std::size_t faultCount() const noexcept;    // within faultWindow
  };
  struct RegistryKey { std::string hostId; ExtensionWorld world; std::string configDigest; std::string moduleDigest;
                       bool operator==(RegistryKey const&) const = default; };
  [[nodiscard]] RegistryKey registryKeyOf(ExtensionSpec const& spec, ExtensionWorld world);   // SHA-256, hex (D23)
  [[nodiscard]] wasm::ModuleIdentity moduleIdentityOf(RegistryKey const& key);  // hostId, world, configDigest; not the module
  class ExtensionRegistry {
  public:
      ExtensionRegistry(core::IClock& clock, ExtensionLimits limits);
      /// The live entry for @p key on @p engine, or loads one (once, however many ask at once: D22).
      [[nodiscard]] std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> acquire(
          Engine& engine, RegistryKey const& key, std::vector<std::byte> bytes);
      void resetBreakers() noexcept;
      [[nodiscard]] std::size_t liveEntries() const;
  };
  // CompiledExtension::load(Engine&, wasm::ModuleIdentity, std::vector<std::byte>, std::shared_ptr<HostBreaker> breaker /* nullable */);
  // CompiledExtension::admitHost() -> std::expected<HostAdmission, ExtensionError>;  recordHostFault(HostAdmission) noexcept
  // prepareExtensions(ExtensionHostConfig, std::expected<std::shared_ptr<Engine>, ExtensionError>, ExtensionRegistry* registry /* nullable */);
  // ExtensionRuntime::configurationReloaded() noexcept { _registry.resetBreakers(); }
  }
  ```

- [ ] **Step 1: Write the breaker and registry tests** (`ExtensionRegistry_test.cpp`; `core::ManualClock`, `FakeWasmBackend`):
  - `"ExtensionRegistry.breakerOpensAtTheLimit"`: 3 faults, each with a fresh admission, within 60 s → `state() == Open`, `admit().error() == HostUnavailable`.
  - `"ExtensionRegistry.faultsAgeOut"`: fault, `clock.advance(61s)`, fault, fault → `Closed`.
  - `"ExtensionRegistry.staleAdmissionCountsOnce"`: 10 × `recordHostFault(sameAdmission)` → `faultCount() == 1`.
  - `"reload resets breaker"`: open it, `registry.resetBreakers()` → `Closed`, `admit()` succeeds, `recordHostFault(oldAdmission)` leaves `faultCount() == 0`.
  - `"rebuild with unchanged bytes reuses the entry"`: two `SessionExtensions::rebuild` with the same declared extensions via a `QueuedOwner` → `fake.loads() == 1`, `registry.liveEntries() == 1`; changed bytes → `loads() == 2`.
  - `"ExtensionRegistry.differentConfigGetsItsOwnEntry"`: same path, different `ConfigMap` → two entries, two different `fake.lastIdentity()` keys.
  - `"registry drop races instantiate"` (`[threading]`): 200 rounds; acquire `first`, a jthread drops it while the test thread acquires `second` and runs `Instance::create(engine, **second, …)`; `REQUIRE(second)`, `CHECK(instance)`, after the join `CHECK(registry.liveEntries() <= 1)`. TSan-clean.
- [ ] **Step 2: Write the remote tests** (`RegistryRemote_test.cpp`; `InProcessHostConnector`, a real statusline test module):
  - `"one death counts once across panes"`: 10 `Instance`s of one entry, `connector.terminateAll()`; every `render({10, 1})` → `HostUnavailable`; then `breaker.faultCount() == 1`, `connector.connects() == 1`; the next `Instance::create` → `connects() == 2`.
  - `"breaker open fails fast without spawning"`: 3 deaths, each followed by a new instance; then `Instance::create(...).error() == HostUnavailable` with `connects()` unchanged.
  - `"registry drop races instantiate (remote)"`: as Step 1 over the real helper; afterwards `connector.liveHosts() == 0` once every reference is gone.
- [ ] **Step 3: Build; expect compile failures.** `… --target vtextension_test vtextension_remote_test`.
- [ ] **Step 4: Implement.**
  - `HostBreaker`: mutex, `std::deque<SteadyTimePoint>` pruned like `Runner::fault`, epoch, state; Open until `reset()`.
  - `CompiledExtension` holds a nullable `shared_ptr<HostBreaker>`; without one `admitHost()` always succeeds.
  - `Instance::create` admits first (open → `HostUnavailable` before instantiating) and keeps the admission; every `HostUnavailable` from instantiate or `callExport` reports it once per instance; Runner's own counting unchanged (D20).
  - `ExtensionRegistry`: one mutex around `RegistryKey` → loading (`std::shared_future<std::expected<std::shared_ptr<CompiledExtension>, ExtensionError>>`) or live (`std::weak_ptr`); an entry whose engine is not `&engine` is a miss; expired entries swept on every acquire; loads run outside the lock.
  - `prepareExtensions` uses `registry->acquire(engine, registryKeyOf(spec, ExtensionWorld::StatusLine), …)` when non-null. `ExtensionRuntime` builds `ExtensionRegistry { _pool.clock(), limits }`, declared before `_engines` so entries die before the engine provider.
  - `ContourGuiApp::onConfigurationReloaded` and the daemon's reload call `configurationReloaded()` on the started runtime, if any.
- [ ] **Step 5: Run.** `out/clang-asan/src/vtextension/vtextension_test "ExtensionRegistry.*" "reload resets breaker" "rebuild with unchanged bytes reuses the entry" "registry drop races instantiate" "[threading]"` and `out/clang-asan/src/vtextension/wasm/remote/vtextension_remote_test "one death counts once across panes" "breaker open fails fast without spawning" "registry drop races instantiate (remote)"` → PASS.
- [ ] **Step 6: Run the whole suites.** `ctest --preset=clang-asan -R vtextension` → PASS.
- [ ] **Step 7: Commit.** `git commit -s -m "feat(vtextension): load each extension once per process, behind a breaker for its host"`

