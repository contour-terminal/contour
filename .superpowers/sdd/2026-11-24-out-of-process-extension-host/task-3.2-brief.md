### Task 3.2: `contour-extension-host` executable

**Files:**
- Create: `src/contour-extension-host/{CMakeLists.txt,main.cpp}`
- Create: `src/vtextension/wasm/host/{HostProcess.hpp,HostProcess.cpp,HostBootstrap.hpp,HostBootstrap_posix.cpp,HostBootstrap_win32.cpp,HelperEngine.hpp,HostProcess_test.cpp}`
- Create: `src/vtextension/wasm/wasmtime/HelperEngine.cpp` (target `vtextension_wasmtime_helper_engine`, D32/D33)
- Create: `src/vtextension/wasm/remote/process/{HelperLocation.hpp,HelperLocation.cpp,HelperLocation_test.cpp}` (D54)
- Create: `src/vtextension/wasm/remote/Helper_test.cpp` and target `vtextension_helper_test` in `wasm/remote/CMakeLists.txt` (only `if(TARGET contour-extension-host)`; links `vtextension vtextension_remote vtextension_remote_process vtextension_remote_wire glaze::glaze core::testing Catch2::Catch2 contour_test_trace`, no engine; `VTEXTENSION_HELPER_PATH`, `add_dependencies`)
- Modify: `wasm/host/{HostServer.hpp,HostServer.cpp,IHostedEngine.hpp (EngineFactory moves here),InProcessHost.hpp,HostServer_test.cpp,CMakeLists.txt}`, `wasm/remote/process/testing/LaunchProbe.cpp` (reports caps), `src/CMakeLists.txt` (`add_subdirectory(contour-extension-host)` after `vtextension`)

**Interfaces:**
- Consumes: 3.1 (`HelperArguments`, `Channel::adopt`, `ControlPage::adopt`, `WakeReceiver::adopt`, `makeSystemProcessLauncher`).
- Produces:
  ```cpp
  namespace vtextension::wasm::host {
  using EngineFactory = std::function<std::unique_ptr<IHostedEngine>()>;          // moved from InProcessHost.hpp
  [[nodiscard]] std::unique_ptr<IHostedEngine> makeHelperEngine();               // HelperEngine.hpp; defined by the engine dir
  // HostServerConfig gains:
  //   std::function<remote::proto::LockdownState()> confine;   ///< After every lane's thread started, before Hello; its
  //                                                            ///< result is Hello's lockdown. Null: `lockdown` is sent.
  enum class HostProcessExit : uint8_t { Served, BadArguments, HandlesUnusable, ParentGone, EngineUnavailable,
                                         ProtocolViolation, ExecutorUnavailable };
  struct HostProcessExitRow { HostProcessExit exit; int code; std::string_view text; };   // text: user-readable, names no engine
  [[nodiscard]] std::span<HostProcessExitRow const> hostProcessExits() noexcept;           // codes 0, 70..75
  [[nodiscard]] int exitCodeOf(HostProcessExit exit) noexcept;
  [[nodiscard]] HostProcessExit hostProcessExitOf(ServerExit exit) noexcept;  // PeerClosed, Stopped → Served
  struct HostEndpoints { remote::transport::Channel control; std::vector<remote::transport::Channel> lanes;
                         remote::transport::ControlPage controlPage; remote::transport::WakeReceiver wake; };
  [[nodiscard]] std::expected<HostEndpoints, HostProcessExit> adoptEndpoints(remote::transport::HelperArguments const& arguments);
  /// A host's whole serving side over its endpoints: the server (3.4 adds the interrupt listener).
  class HostProcess {
  public:
      HostProcess(IHostedEngine& engine, HostEndpoints endpoints, HostServerConfig config);
      [[nodiscard]] ServerExit run();      // once
      void stop() noexcept;                // thread-safe, idempotent
  };
  /// The executable's body: parse, adopt, bind to the parent, make the engine, serve.
  [[nodiscard]] int runExtensionHost(std::span<std::string_view const> arguments, EngineFactory const& engines);
  enum class BootstrapError : uint8_t { ParentGone, Refused };
  [[nodiscard]] std::expected<void, BootstrapError> bindToParent(int64_t parentProcessId) noexcept;     // D37
  enum class CapPolicy : uint8_t { Skip, Apply };
  inline constexpr CapPolicy BuildCapPolicy = /* Skip under CONTOUR_SANITIZE (compile definition), else Apply */;
  [[nodiscard]] remote::proto::LockdownState capResources(uint64_t dataBytes, CapPolicy policy) noexcept;
      // Linux: RLIMIT_NPROC 0, RLIMIT_DATA dataBytes (0: none); FreeBSD: RLIMIT_NPROC 0; macOS, Windows: nothing.
      // Unsandboxed on success, Failed if a setrlimit failed.
  }
  namespace vtextension::wasm::remote::process {
  enum class HelperLocationError : uint8_t { NotFound };
  inline constexpr std::string_view ExtensionHostName = /* "contour-extension-host", ".exe" on Windows */;
  [[nodiscard]] std::expected<std::filesystem::path, HelperLocationError> locateExtensionHost(
      std::filesystem::path const& selfExecutable, std::function<bool(std::filesystem::path const&)> const& isExecutable);
  }
  ```
  `main.cpp`: `return host::runExtensionHost(arguments-as-string_views, &host::makeHelperEngine);` — nothing else.
  Exit table: Served 0, BadArguments 70, HandlesUnusable 71, ParentGone 72, EngineUnavailable 73, ProtocolViolation 74, ExecutorUnavailable 75.

- [ ] **Step 1: Write the server and process tests.**
  - `HostServer_test.cpp`: `"HostServer.confineRunsAfterTheLanesStartAndBeforeHello"` — a counting `startExecutor` and a `confine` that records the count and returns `LockdownState::Failed`: count at confine == `MaxLaneChannels`, the first control PDU is `Hello{ …, lockdown = Failed }`; `"HostServer.noConfineSendsTheConfiguredLockdown"` (unchanged behaviour).
  - `HostProcess_test.cpp` (vtextension_remote_test): `"HostProcess.servesOverAdoptedEndpoints"` — channel pairs, a page and a wake pair; the helper ends' duplicates formatted into `HelperArguments` (`parentProcessId` = own pid); `runExtensionHost(args, testing::makeHostedEngine)` on a jthread → the parent reads `Hello{ ProtocolVersion, MemoryMode::Copy, LockdownState::Unsandboxed }`; closing the control end → the jthread's result is `0`. `"HostProcess.badArgumentsExit"` (`{"--bogus=1"}` → 70, the engine factory never called); `"HostProcess.unusableHandleExits"` (`--control=` a closed descriptor → 71); `"HostProcess.wrongParentExits"` (`parentProcessId` = own pid + 1 on POSIX → 72, before any engine is made); `"HostProcess.exitCodesAreDistinct"` (every row's code unique, none in `{1, 23, 66}`, `Served` is 0).
  - `HelperLocation_test.cpp` (vtextension_process_test), a table over a fake `isExecutable`: Unix `/usr/bin/contour` → `/usr/bin/<rel>/contour-extension-host` (rel from the compile definition `CONTOUR_EXTENSION_HOST_RELATIVE_DIR`); only the same-directory candidate existing → that one; macOS `…/Contour.app/Contents/MacOS/contour` → `…/Contents/Helpers/contour-extension-host`; Windows `C:\Contour\bin\contour.exe` → `…\bin\contour-extension-host.exe`; none → `NotFound`.
  - `Helper_test.cpp` (vtextension_helper_test, the real executable through `makeSystemProcessLauncher()`): `"Helper.saysHelloOverInheritedChannels"` (Hello as above; close control → `ExitStatus{ Exited, 0 }` within 5 s); `"Helper.exitsOnBadArguments"` (→ `{ Exited, 70 }`).
  - `ProcessLauncher_test.cpp`: `"HostBootstrap.capsHold"` (the probe now applies `capResources(64 MiB, BuildCapPolicy)` and reports `nproc=`/`data=`; Linux with `Apply`: `nproc=0`, `data=67108864`; under `Skip`: `SKIP("sanitizer builds apply no caps")`); `"HostBootstrap.wrongParentExits"` (probe with `--parent=1` → `{ Exited, 72 }` on Linux/FreeBSD/macOS).
- [ ] **Step 2: Build; expect compile failure.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_remote_test vtextension_process_test`.
- [ ] **Step 3: Implement the server hook.** `HostServer::run()` order becomes: start the executors (failure → `ExecutorUnavailable`, nothing sent), `confine()` if set, send `Hello`, serve control.
- [ ] **Step 4: Implement `HostProcess` and `runExtensionHost`.** `runExtensionHost`: parse (`BadArguments`), `bindToParent` (`ParentGone`), `adoptEndpoints` (`HandlesUnusable`: any adopt failure, `lanes.size() != MaxLaneChannels`), engine (`EngineUnavailable` on null), `HostServerConfig{ .confine = [&]{ return capResources(args.dataBytes, BuildCapPolicy); } }`, `HostProcess::run()` → `hostProcessExitOf`. Each non-`Served` exit writes one line `contour-extension-host: <text>` to stderr. Under `CapPolicy::Skip` it writes once `contour-extension-host: resource caps are off in this build`. `bindToParent` and `capResources` per D37/D38 in `HostBootstrap_posix.cpp` (`#if` per OS for `prctl`/`procctl`/`RLIMIT_DATA`) and `HostBootstrap_win32.cpp` (both no-ops returning success / `Unsandboxed`).
- [ ] **Step 5: The executable and its engine.** `vtextension_wasmtime_helper_engine` (`HelperEngine.cpp`: one process-wide `wasmtime::makeKindState()`, `makeWasmtimeEngine(kind, { .trapMode = HelperTrapMode })`, `HelperTrapMode` from the D33 compile definition). `src/contour-extension-host/CMakeLists.txt`: `if(TARGET vtextension_wasmtime)`: `add_executable(contour-extension-host main.cpp)` linking `vtextension_host vtextension_wasmtime_helper_engine contour::wasmtime`; on Windows `WIN32_EXECUTABLE OFF` (console subsystem, no window: the launcher passes `CREATE_NO_WINDOW`). Otherwise a status line: `contour-extension-host is not built: it needs Wasmtime until its WAMR engine lands`.
- [ ] **Step 6: Run.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target contour-extension-host vtextension_helper_test vtextension_remote_test vtextension_process_test extension_host_probe`; then `out/clang-asan/src/vtextension/wasm/remote/vtextension_remote_test "HostServer.*" "HostProcess.*"`, `…/vtextension_process_test "HelperLocation.*" "HostBootstrap.*"`, `…/vtextension_helper_test "Helper.*"` → PASS; `ctest --preset=clang-asan -R check_wasm_runtime_containment` → PASS.
- [ ] **Step 7: Commit.** `git commit -s -m "feat(vtextension): the contour-extension-host executable"`
- [ ] **Step 8: Install rules (separate commit).** In `src/contour-extension-host/CMakeLists.txt` (D54): `include(GNUInstallDirs)`; non-Windows non-bundle `install(TARGETS contour-extension-host RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/contour")`; Windows `install(TARGETS contour-extension-host RUNTIME DESTINATION bin)`; `CONTOUR_MACOS_APP_BUNDLE`: a POST_BUILD copy into `$<TARGET_BUNDLE_CONTENT_DIR:contour>/Helpers/` (the bundle install carries it). `file(RELATIVE_PATH …)` → `CONTOUR_EXTENSION_HOST_RELATIVE_DIR` on `vtextension_remote_process`. Verify: `DESTDIR=<scratchpad>/install cmake --install out/clang-asan` → `<scratchpad>/install/<prefix>/libexec/contour/contour-extension-host` exists. Commit: `git commit -s -m "build: install contour-extension-host beside contour"`

