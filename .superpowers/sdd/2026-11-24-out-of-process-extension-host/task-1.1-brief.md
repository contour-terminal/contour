### Task 1.1: Pin Wasmtime and verify the C API facts the design relies on

**Files:**
- Modify: `cmake/ContourThirdParties.cmake` (new block after the WAMR block)
- Modify: `CMakeLists.txt:170-185` (new internal flag)
- Modify: this plan, section "Phase-1 facts" (appended, local only)

**Interfaces:**
- Produces: CMake target `contour::wasmtime` (INTERFACE, linking the static `libwasmtime.a`, its include
  dir, and the platform's system libs: `pthread dl m` on Linux, `ws2_32 advapi32 userenv ntdll shell32
  ole32 bcrypt` on Windows). The variable `CONTOUR_HAVE_WASMTIME` (ON when resolved).

- [ ] **Step 1: Find the LTS version and archive names.** Open
  `https://github.com/bytecodealliance/wasmtime/releases`. Take the newest `v48.0.x` and record the
  `c-api` archive names for x86_64-linux, aarch64-linux, x86_64-macos, aarch64-macos and x86_64-windows
  (msvc), with their SHA-256 (download and `sha256sum` each). If v48 is not marked LTS, stop and report.
- [ ] **Step 2: Verify the API surface in the archive's headers.** Grep `include/wasmtime/*.h` for each
  symbol and record "present / absent / differently named" in "Phase-1 facts":
  `wasmtime_config_host_memory_creator_set`, `wasmtime_config_epoch_interruption_set`,
  `wasmtime_store_epoch_deadline_callback`, `wasmtime_context_set_epoch_deadline`,
  `wasmtime_engine_increment_epoch`, `wasmtime_config_signals_based_traps_set`,
  `wasmtime_config_macos_use_mach_ports_set`, `wasmtime_config_max_wasm_stack_set`,
  `wasmtime_store_limiter`, `wasmtime_module_serialize`, `wasmtime_module_deserialize`,
  `wasmtime_config_parallel_compilation_set`, `wasmtime_config_cranelift_opt_level_set`, and one setter
  per feature in Global Constraints (`wasmtime_config_wasm_*_set`). An absent symbol that the design
  needs (memory creator, epoch, signals-based traps) stops the phase: report to the owner.
- [ ] **Step 3: Add the CMake resolution.** With `CONTOUR_USE_CPM=ON` and a pinned archive for the
  host platform: `CPMAddPackage(NAME wasmtime URL <archive> URL_HASH SHA256=<hash> DOWNLOAD_ONLY YES)`,
  then define `contour::wasmtime`. Otherwise `find_package(wasmtime 48 QUIET)`. Otherwise
  `CONTOUR_HAVE_WASMTIME=OFF`, with `message(STATUS "Extension host engine: WAMR (<reason>)")`. Print the
  outcome in the summary block next to `|> WebAssembly runtime:`.
- [ ] **Step 4: Configure and check.** Run:
  `cmake --preset clang-asan -B out/clang-asan 2>&1 | grep -i 'wasmtime\|Extension host engine'`.
  Expected: a line showing Wasmtime 48.x resolved. `ls out/clang-asan/_deps/wasmtime-src/lib/libwasmtime.a`
  exists.
- [ ] **Step 5: Commit.**
  `git commit -s -m "build: fetch a pinned Wasmtime for the extension host"`

