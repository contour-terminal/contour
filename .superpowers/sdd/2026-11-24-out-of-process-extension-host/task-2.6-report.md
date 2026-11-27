# Task 2.6 report: `ExtensionRegistry` and the per-extension `HostBreaker`

Status: DONE. Commit `45c31a567 feat(vtextension): load each extension once per process, behind a breaker for its host` (on top of 3ce6b5120).

## What changed

New:
- `src/vtextension/HostBreaker.{hpp,cpp}`: `BreakerState`, `HostAdmission{epoch}`, `HostBreaker(IClock&, faultsBeforeDisable, faultWindow)` with `admit()`, `recordHostFault(admission)`, `reset()`, `state()`, `faultCount()`. It holds a mutex, a deque of fault times pruned the way `Runner` prunes its faults, an epoch, and the state. A fault counts only if `admission.epoch == _epoch`, and counting it advances the epoch. `reset()` closes the breaker, clears the faults and advances the epoch.
- `src/vtextension/ExtensionRegistry.{hpp,cpp}`: `ExtensionWorld{StatusLine}` (names come from a one-row table), `RegistryKey`, `registryKeyOf(spec, world)`, `moduleIdentityOf(key)` and `ExtensionRegistry`.
  - The config digest follows D23: SHA-256 over length-prefixed fields (an 8-byte LE length, then the bytes). The fields are `runCommand` (one byte), then the settings stably sorted by name, name and value each as a field. The module digest is SHA-256 of the bytes. Both are lower-case hex.
  - `moduleIdentityOf` = `"<world>\n<configDigest>\n<hostId>"`. The module digest is not part of it, so a changed module keeps its helper key.
  - `ExtensionRegistry` keeps one mutex around `vector<Entry>`. Each entry has the key, an `Engine const*` (used only for comparison), a serial, `shared_ptr<HostBreaker>`, and a state that is `variant<shared_future<LoadResult>, weak_ptr<CompiledExtension>>`. Every acquire sweeps expired entries. An entry on another engine counts as a miss. An entry still loading is waited on outside the lock.
  - A new entry's load runs outside the lock. `settle()` then either publishes it as a weak ref or erases it on failure, so the next acquire retries. Only after that is the promise fulfilled, so the map's future never holds the strong value.
  - If the load throws, the entry is erased and the exception is set on the promise and rethrown.
  - `resetBreakers()` resets the breaker of every entry still listed, whether reused or replaced. `liveEntries()` counts entries that are loading or unexpired.
- Tests: `src/vtextension/ExtensionRegistry_test.cpp` (vtextension_test) and `src/vtextension/wasm/remote/RegistryRemote_test.cpp` (vtextension_remote_test).

Modified:
- `CompiledExtension.{hpp,cpp}`: `load(Engine&, ModuleIdentity, bytes, shared_ptr<HostBreaker> breaker)` (nullable). Adds `admitHost()` (always admits when there is no breaker), `recordHostFault(admission)` and `breaker()` (a const pointer, for tests and diagnostics). All are const because every pane shares the module; the breaker is thread-safe.
- `Instance.{hpp,cpp}`: `create` admits first. While the breaker is open it returns `HostUnavailable` before anything is instantiated. The admission goes into the constructor and is kept as `optional<HostAdmission>`. `reportHostFault(error)` reports `HostUnavailable` once per instance, using `std::exchange`. It runs on an instantiate failure in `create` and when `guarded()` records a fault. `Runner` is unchanged (D20).
- `ExtensionHost.{hpp,cpp}`: `prepareExtensions(config, engine, ExtensionRegistry* registry)`. With a registry it calls `registry->acquire(engine, registryKeyOf(spec, StatusLine), bytes)`, computing the key before the bytes move. With null it loads as before (identity = hostId, no breaker).
- `SessionExtensions.{hpp,cpp}`:
  - `SessionExtensionServices::registry` (a reference). `BuildResult::prepare` passes it on.
  - `ExtensionRuntime` owns `ExtensionRegistry _registry { _pool.clock(), limits }` and gains `configurationReloaded() noexcept { _registry.resetBreakers(); }`.
- `src/contour/ContourGuiApp.cpp`: under `#ifdef CONTOUR_EXTENSIONS`, `onConfigurationReloaded()` calls `startedExtensionRuntime()->configurationReloaded()` when the runtime has started.
- Daemon (`src/vthost`):
  - `IExtensionHosting::configurationReloaded() noexcept`, new pure virtual.
  - `DaemonExtensions` forwards it to its runtime if that has started.
  - `SessionHost::replaceProfiles()` calls it for a non-null catalog. This is the method the daemon's `ConfigReloadWatcher` drives.
  - `FakeExtensionHosting` counts `reloads`; the `KeeperFixture` stub is a no-op.
  - New test: `"replaceProfiles tells the extensions of the reload"`.
- Mechanical: every other `CompiledExtension::load` caller passes `nullptr` (test helpers, `RenderMeasurement.hpp`, `ControlImports_test`, `Instance_test`). Every other `prepareExtensions` caller passes `nullptr` (bench-headless, ExtensionHost_test, SessionExtensions_test). The test fixtures of `SessionExtensions_test` and `Isolation_test` gained a registry. CMake sources and headers were added, and `PublicHeaders.cpp` includes both new headers.

## Tests (TDD)

RED: the first build of the new test files failed with `'vtextension/ExtensionRegistry.hpp' file not found`, as expected.

GREEN, clang-asan:
- `vtextension_test "[registry]"`: 16 cases / 910 assertions pass. These are all brief names verbatim: `ExtensionRegistry.breakerOpensAtTheLimit`, `.faultsAgeOut`, `.staleAdmissionCountsOnce`, `reload resets breaker`, `rebuild with unchanged bytes reuses the entry`, `ExtensionRegistry.differentConfigGetsItsOwnEntry`, `registry drop races instantiate` (`[threading]`, 200 rounds).
  - Extra cases: key order/length-prefix/runCommand/module-identity, per-engine entries, a failed load leaves no entry, a dropped entry is loaded again, an open breaker refuses before instantiating, an instance reports its host's death once, a trap is no host death, `ExtensionRuntime.configurationReloadedResetsEveryBreaker`, `ExtensionRegistry.concurrentAcquiresLoadOnce` (`[threading]`).
- `vtextension_remote_test "[registry]"`: 3 cases / 849 assertions pass: `one death counts once across panes`, `breaker open fails fast without spawning`, `registry drop races instantiate (remote)`.
- Full binaries: vtextension_test 382 cases / 13223 assertions, vtextension_remote_test 181 cases / 4130 assertions, all pass. `ctest --preset=clang-asan -R vtextension` passed 3/3.
- `vthost_test "[reload]"`: 5 cases pass. The full `vthost_test` passes under ctest (130 s).
- bench_headless_* smoke tests: 11/11 pass. `bench_latency_test` was "Not Run" because I did not build that binary; it is unrelated to this change.
- TSan (`out/clang-tsan`, -j 4 niced): `vtextension_test "[registry]"` (16 cases), `vtextension_test "[threading]"` (19 cases / 1101 assertions) and `vtextension_remote_test "[registry]"` (3 cases) all pass with no ThreadSanitizer report.
- Built warning-free under -Werror: `vtextension_test vtextension_remote_test vthost_test bench-headless contour`.

clang-tidy, run by hand: `clang-tidy -p out/clang-asan --quiet src/vtextension/{ExtensionRegistry,HostBreaker,CompiledExtension,Instance,ExtensionHost,SessionExtensions,ExtensionRegistry_test,SessionExtensions_test,Isolation_test}.cpp src/vtextension/wasm/remote/RegistryRemote_test.cpp src/vthost/{DaemonExtensions,SessionHost}.cpp src/vthost/testing/KeeperFixture.cpp src/contour/ContourGuiApp.cpp`. It reported one finding: `readability-ambiguous-smartptr-reset-call` on `entry.breaker->reset()`. I fixed it by binding a reference first, and the files are now clean. `/usr/bin/clang-format` and `editorconfig-checker` are clean on every changed file.

## Deviations from the brief

1. **`HostBreaker` has its own header, `HostBreaker.hpp/.cpp`.** The brief put it in `ExtensionRegistry.hpp`, but `CompiledExtension.hpp` needs `HostBreaker`/`HostAdmission`, and `ExtensionRegistry.hpp` needs `CompiledExtension`. A separate header avoids the include cycle. The tests remain in `ExtensionRegistry_test.cpp`.
2. **`_registry` is declared after `_pool` in `ExtensionRuntime`, not before `_engines`.** It is built from `_pool.clock()`, so declaring it earlier would call into a pool that is not yet constructed. It is still destroyed before both the pool and `_engines`, so the entries go before the engine provider. That is the intent the brief states.
3. **`CompiledExtension::load` and `prepareExtensions` take the new breaker/registry argument with no default.** This follows the D16 rule against default arguments, so every caller now passes `nullptr` explicitly.
4. **I added a daemon reload hook.** On this branch the daemon's reload is `ConfigReloadWatcher` → `SessionHost::replaceProfiles`. Its extensions are not rebuilt on reload, and the runtime lives behind `IExtensionHosting`. I added `IExtensionHosting::configurationReloaded()` and wired it through `DaemonExtensions`.
5. **`ExtensionSpec` is forward-declared in `ExtensionRegistry.hpp`, which takes it only by reference.** This keeps `ExtensionHost.hpp` out of the registry's header.

## Concerns

- `core::Sha256` is a scalar implementation, and the module digest hashes the whole module on every build (up to 16 MiB). That costs a few ms for typical modules and roughly 100 ms at the cap, on the build thread only, never on the GUI thread. D22 chose this. If it shows up, a cheaper check before hashing (size + mtime) could skip it.
- D20's bounded undercount is as specified. An instance admitted after a death but before that death's first report can make a second death count as stale. This happens at most once per race.
- The GUI wiring has no test of its own: `onConfigurationReloaded` is a two-line forward. `ExtensionRuntime::configurationReloaded` is covered in vtextension, and the daemon forward is covered in `vthost_test`.
- In the remote race test, the last reference to a module is sometimes dropped on the dropper jthread. That is fine for `RemoteBackend`, which posts `Release` on the control channel from any thread. Production keeps dropping on owner threads.

## Fix round 1

Commit `4a0b5086d test(vtextension): pin that callers share a load in flight, and its failure`.

1. (Important) Shared-load path tested.
   - **Fake gate:** `FakeWasmBackend::loadGate` (`std::function<void()>`) runs on the loading thread after the load is counted and before `loadFailure` is read.
   - **Test hook:** `ExtensionRegistry::waitingAcquires()` counts acquires waiting for another call's load. It is an atomic, decremented on every exit by a unique_ptr deleter, so an exception thrown by the load is covered too.
   - **New `[threading]` cases** (both run through `shareOneLoad()`):
     - `ExtensionRegistry.waitersShareALoadInFlight`: all 5 calls get the same pointer; `loads()==1`, `liveEntries()==1`.
     - `ExtensionRegistry.aFailedLoadInFlightFailsEveryWaiter`: the loader and all 4 waiters get `InvalidComponent`; `liveEntries()==0`; the next acquire loads again (`loads()==2`).
   - **Shared setup in `shareOneLoad()`:**
     - It holds the first load and starts 4 waiters.
     - It then asserts `waitingAcquires()==4` and `loads()==1`, then opens the gate.
     - The spin it waits with has a 10 s real-time give-up bound. A passing case never reaches it; the bound only turns a "loads under its lock" regression into a failure instead of a hang.
   - **Mutation check:** I treated an in-flight load as a miss (`entry != end && !holds_alternative<shared_future>`). Both new cases then failed (11 failed assertions). Source restored.
2. (Minor) Reworded the reload comments.
   - **`ContourGuiApp::onConfigurationReloaded`:** a pane built or reconfigured afterwards may start the extension again; a pane whose runner disabled it stays disabled, because an unchanged reload does not rebuild it.
   - **`SessionHost::replaceProfiles`:** a reload rebuilds no running session.
3. (Minor) Module digest memo: **not done.** On this branch, one rebuild pass is one pane: every `SessionExtensions::rebuild` posts its own build job, and `prepareExtensions` sees a single pane's extensions. So a per-pass memo would only help one pane declaring one module path twice. It cannot make N restored panes hash each module once, and `ExtensionSpec` carries no path to key it by.
   - **Ways to actually get one hash per module across panes:**
     - (a) A memo shared by the build jobs queued together, alive while the single build thread has work queued. This needs an idle signal from the executor.
     - (b) Keeping the bytes of live entries, to compare instead of hash. Memory cost: up to 16 MiB per entry.
     - (c) A fast non-cryptographic digest.

   The cost meanwhile stays on the build thread, never the GUI thread. This needs a controller decision.
4. (Minor) Fixed the `Instance::create` `@return` doc.
5. (Minor) `differentConfigGetsItsOwnEntry` now uses two pairs and acquires them in reversed order, so "in another order" is real.

Re-run:
- ASan, all pass: vtextension_test 384 cases / 13255 assertions; vtextension_remote_test 181 / 4130; vthost_test `[reload]` 5 cases; `ctest -R vtextension` 3/3. The build of `vtextension_test vtextension_remote_test vthost_test contour` is warning-free.
- TSan, no reports: vtextension_test `[registry]` 18 cases / 942 assertions, `[threading]` 21 / 1133; vtextension_remote_test `[registry]` 3 / 849.
- clang-tidy (`-p out/clang-asan --quiet` on ExtensionRegistry.cpp and ExtensionRegistry_test.cpp): clean. clang-format and editorconfig-checker: clean.
