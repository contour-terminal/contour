### Task 1.7: Phase 1 gate

- [ ] **Step 1: Run the full suites under ASan.** Run:
  `ctest --preset=clang-asan -R 'vtextension'`. Expected: all pass, including both contract suites.
- [ ] **Step 2: Run the threaded cases under TSan.** Run:
  `cmake --preset clang-tsan && nice -n 19 ionice -c 3 cmake --build --preset clang-tsan -j 6 --target vtextension_wasmtime_test`,
  then `out/clang-tsan/src/vtextension/wasm/wasmtime/vtextension_wasmtime_test "[wasm-contract]"`.
  Expected: no reports.
- [ ] **Step 3: Measure the speed-up in-process.** In a `clang-release` tree, add a hidden
  `[.measure]` case in `WasmtimeBackend_test.cpp` that runs a bundled extension's `render(120, 1)`
  1000 times. Use `plasma` (the heaviest on master) if `CONTOUR_WASM_GUESTS` is on, otherwise the
  `TestModules` loop. Report the median per call for WAMR and for Wasmtime, three runs each. Record the
  numbers in "Phase-1 facts". There is no pass threshold, but a speed-up below 3× is reported to the owner
  before phase 2 starts.
- [ ] **Step 4: Run `/code-review` on the phase diff and address every finding.**
- [ ] **Step 5: Write the phase summary** (performance numbers, risk, coverage of `wasm/wasmtime/` and
  `wasm/wasi/` via the `clang-coverage` preset).

### Phase-1 facts

*(Filled in by Tasks 1.1, 1.5 and 1.7.)*

**Task 1.1 — Wasmtime pin (verified 2026-11-24).**

- LTS policy (upstream `docs/stability-release.md`): every major divisible by 12 is LTS, 24 months; two LTS
  lines supported at once (today 36 and 48). Newest LTS patch: **v48.0.10** (2026-11-19). Newest overall:
  v51.0.0. v48 is the current LTS, as the spec assumed.
- Pinned archives (`wasmtime-v48.0.10-<name>`), SHA-256 from `sha256sum`, identical to GitHub's asset digest:

  | Platform | Archive | SHA-256 |
  |---|---|---|
  | Linux x86_64 | `x86_64-linux-c-api.tar.xz` | `5fc9a0b8c825ae7e2730f41f6260ec38556236f28bcd34fbb9b76e15c9f56e27` |
  | Linux aarch64 | `aarch64-linux-c-api.tar.xz` | `7be1929dd01708212668696cf55b224b4c82bd562bc231596122563e2eb32a3d` |
  | macOS x86_64 | `x86_64-macos-c-api.tar.xz` | `3bd16df4a8beedb510e8b92a9a3063da8b4187a6ebbc4f363b25397d436ef232` |
  | macOS aarch64 | `aarch64-macos-c-api.tar.xz` | `868e59b586157b2497d5cd3004a588dde85c8542b4ff2366acc069a7da5d6927` |
  | Windows x86_64 (MSVC) | `x86_64-windows-c-api.zip` | `2c5cd1a231c0b6aee80ecc700d68946bca4afaa09b728f6f8c43db64207ae682` |

- Archive layout (single top dir, stripped by CPM): `include/{wasm.h,wasi.h,wasmtime.h,wasmtime/*.h,*.hh}`,
  `lib/libwasmtime.a` (68 MiB, x86_64-linux) + `lib/libwasmtime.so` (30 MiB); Windows `lib/wasmtime.lib`
  (static, 79 MiB) + `wasmtime.dll` + `wasmtime.dll.lib`. Every archive also carries a `min/` build
  (`min/include`, `min/lib`) — Windows `min/lib/wasmtime.lib` is 2.5 MiB: a runtime-only build without
  the compiler, a candidate for a deserialize-only helper later.
- Default archive's `include/wasmtime/conf.h` enables: PROFILING, WAT, CACHE, PARALLEL_COMPILATION, WASI,
  WASI_HTTP, LOGGING, COREDUMP, ADDR2LINE, DEMANGLE, THREADS, GC, GC_DRC, GC_NULL, ASYNC, CRANELIFT, WINCH,
  DEBUG_BUILTINS, POOLING_ALLOCATOR, COMPONENT_MODEL, COMPONENT_MODEL_ASYNC, PULLEY. Our config switches the
  wasm features off at runtime; the code is still linked.
- Static linking on Windows: `wasm.h` uses `__declspec(dllimport)` unless `LIBWASM_STATIC` is defined;
  `wasi.h` unless `WASI_API_EXTERN` is predefined. `contour::wasmtime` defines `LIBWASM_STATIC` and
  `WASI_API_EXTERN=` there.
- API surface (x86_64-linux headers, and `nm` confirms each as a `T` symbol in `libwasmtime.a`). All
  **present**, none renamed. Most setters are declared via `WASMTIME_CONFIG_PROP(void, <name>, <type>)` in
  `wasmtime/config.h`, so a plain grep for the full name only finds them in the `.hh` wrappers:
  - `wasmtime_config_host_memory_creator_set(wasm_config_t*, wasmtime_memory_creator_t*)` — present
    (config.h:637). `wasmtime_memory_creator_t { void* env; wasmtime_new_memory_callback_t new_memory;
    void (*finalizer)(void*); }`; the config copies the struct, `new_memory` must be thread-safe.
  - `wasmtime_config_epoch_interruption_set(bool)` — present (PROP, config.h:167).
  - `wasmtime_store_epoch_deadline_callback(store, wasmtime_error_t* (*)(wasmtime_context_t*, void*,
    uint64_t* epoch_deadline_delta, wasmtime_update_deadline_kind_t*), void* data, void (*finalizer)(void*))`
    — present (store.h:254). Kinds: `WASMTIME_UPDATE_DEADLINE_CONTINUE` 0, `..._YIELD` 1 (needs async).
  - `wasmtime_context_set_epoch_deadline(context, uint64_t ticks_beyond_current)` — present (store.h:224).
  - `wasmtime_engine_increment_epoch(engine)` — present.
  - `wasmtime_config_signals_based_traps_set(bool)` — present (PROP, config.h:551).
  - `wasmtime_config_macos_use_mach_ports_set(bool)` — present (PROP, config.h:538).
  - `wasmtime_config_max_wasm_stack_set(size_t)` — present (PROP, config.h:182).
  - `wasmtime_store_limiter(store, int64_t memory_size, int64_t table_elements, int64_t instances,
    int64_t tables, int64_t memories)` — present (store.h:111); negative = default.
  - `wasmtime_module_serialize(const wasmtime_module_t*, wasm_byte_vec_t*)` — present (module.h:110).
  - `wasmtime_module_deserialize(wasm_engine_t*, const uint8_t*, size_t, wasmtime_module_t**)` — present
    (module.h:126).
  - `wasmtime_config_parallel_compilation_set(bool)` — present (PROP, config.h:363).
  - `wasmtime_config_cranelift_opt_level_set(wasmtime_opt_level_t)` — present (PROP, config.h:399);
    `WASMTIME_OPT_LEVEL_NONE/SPEED/SPEED_AND_SIZE`, default SPEED.
  - Feature setters, all `(wasm_config_t*, bool)`, present: `wasm_bulk_memory`, `wasm_multi_value`,
    `wasm_reference_types` (ON); `wasm_gc`, `wasm_exceptions`, `wasm_threads`, `wasm_memory64`,
    `wasm_multi_memory`, `wasm_tail_call`, `wasm_simd`, `wasm_relaxed_simd`, `wasm_component_model` (OFF).
    Also present (headers only, not nm-checked) and to be switched OFF explicitly so the table is the whole truth:
    `wasm_function_references` (GC depends on it), `wasm_relaxed_simd_deterministic`, `wasm_wide_arithmetic`,
    `wasm_branch_hinting`, `wasm_custom_page_sizes`, `wasm_stack_switching`, `wasm_component_model_async`,
    `..._async_stackful`, `..._map`, `..._more_async_builtins`.
  - **No setter exists** for sign-extension, saturating float-to-int and mutable globals: Wasmtime
    always accepts them. They are ON in the table anyway, so nothing is lost, but the table's ON rows for
    these three have no Wasmtime switch to drive.
- CMake: `contour::wasmtime` (INTERFACE: `include/` as SYSTEM, the static lib, `pthread dl m` on Linux,
  `ws2_32 advapi32 userenv ntdll shell32 ole32 bcrypt` on Windows, nothing extra on macOS — unverified
  until a macOS link in Task 1.3). `CONTOUR_HAVE_WASMTIME` is a plain (non-cache) variable, initialised OFF
  in `CMakeLists.txt`, set ON in `cmake/ContourThirdParties.cmake`; it is defined only for the top-level
  scope and below.

**Task 1.5 — WASI shim vs WAMR's libc-wasi (measured 2026-11-25).**

- `everyAllowedWasiFunctionWorks` passes on WAMR unchanged: for the in-bounds calls the contract makes,
  WAMR's built-in libc-wasi (stdio bound to the null device, no preopens) returns the same errno as our
  shim for all 15 functions — `fd_close(9)`, `fd_prestat_get(3)`, `fd_prestat_dir_name(3)` → `badf` (8);
  `fd_seek(1, 0, set)` on the null device → success; `proc_exit` → `CallFailure::Trap`. No Traits hook
  was needed.
- Not asserted across engines (they may differ, and the contract does not depend on it): `fd_close` on a
  standard stream (WAMR closes its table entry; the shim keeps the stream), `fd_seek` on a standard stream
  (WAMR seeks the null device; the shim always reports offset 0), the CPU-time clocks (shim: `nosys`;
  WAMR: whatever the host clock gives), and `fd_fdstat_get` rights.

---

## Phase 2 — Protocol, `HostServer`, `RemoteBackend`; identity, registry, breaker

