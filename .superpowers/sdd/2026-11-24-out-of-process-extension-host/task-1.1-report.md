# Task 1.1 report — pin Wasmtime, verify C API facts

Status: DONE_WITH_CONCERNS
Commit: `69a89b1d7 build: fetch a pinned Wasmtime for the extension host`

## Step 1 — LTS and archives

`gh api repos/bytecodealliance/wasmtime/releases` (2026-11-24): newest v51.0.0; maintained lines 50, 49,
48, 36. Upstream `docs/stability-release.md`: "Each release that is a multiple of 12 is considered an LTS
release and is supported for 24 months." So **v48 is the current LTS**; newest patch **v48.0.10**
(2026-11-19).

Downloaded into `/tmp/claude-1000/wasmtime-check/dl/` and hashed; `sha256sum` output equals the GitHub
asset `digest` field for all five:

```
5fc9a0b8c825ae7e2730f41f6260ec38556236f28bcd34fbb9b76e15c9f56e27  wasmtime-v48.0.10-x86_64-linux-c-api.tar.xz
7be1929dd01708212668696cf55b224b4c82bd562bc231596122563e2eb32a3d  wasmtime-v48.0.10-aarch64-linux-c-api.tar.xz
3bd16df4a8beedb510e8b92a9a3063da8b4187a6ebbc4f363b25397d436ef232  wasmtime-v48.0.10-x86_64-macos-c-api.tar.xz
868e59b586157b2497d5cd3004a588dde85c8542b4ff2366acc069a7da5d6927  wasmtime-v48.0.10-aarch64-macos-c-api.tar.xz
2c5cd1a231c0b6aee80ecc700d68946bca4afaa09b728f6f8c43db64207ae682  wasmtime-v48.0.10-x86_64-windows-c-api.zip
```

Layout: single top dir; `include/`, `lib/libwasmtime.a` + `.so`/`.dylib` (Windows: `lib/wasmtime.lib`
static, `wasmtime.dll`, `wasmtime.dll.lib`), plus a `min/` runtime-only build in every archive.

## Step 2 — API surface

All required symbols **present**, none renamed, each also confirmed as a `T` symbol in the x86_64-linux
`libwasmtime.a` via `nm -g --defined-only`:
host_memory_creator_set, epoch_interruption_set, store_epoch_deadline_callback,
context_set_epoch_deadline, engine_increment_epoch, signals_based_traps_set, macos_use_mach_ports_set,
max_wasm_stack_set, store_limiter, module_serialize, module_deserialize, parallel_compilation_set,
cranelift_opt_level_set, and wasm_{bulk_memory,multi_value,reference_types,gc,exceptions,threads,memory64,
multi_memory,tail_call,simd,relaxed_simd,component_model}_set.

Most setters are macro-declared (`WASMTIME_CONFIG_PROP(void, name, type)` in `wasmtime/config.h`), so a
literal grep for the full function name only matches the `.hh` wrappers — a trap for later greps.
Signatures, line numbers and extra feature setters are recorded in the plan's "Phase-1 facts".

Notable: **no setter exists for sign-extension, saturating float-to-int, mutable globals** (Wasmtime always
enables them). They are ON in the design's table, so nothing conflicts; Task 1.2's table just has no
Wasmtime switch for those rows. Further setters exist that the table should switch OFF explicitly:
function_references, relaxed_simd_deterministic, wide_arithmetic, branch_hinting, custom_page_sizes,
stack_switching, component_model_{async,async_stackful,map,more_async_builtins}.

## Step 3 — CMake

- `cmake/ContourThirdParties.cmake`: new block after WAMR. `WASMTIME_VERSION "48.0.10"`, one table
  `_WASMTIME_ARCHIVES` (system|processor|archive|sha256). Processor normalised (amd64→x86_64,
  arm64→aarch64); Windows rows only for MSVC ABI. With CPM and a row: `CPMAddPackage(NAME wasmtime VERSION
  48.0.10 URL … URL_HASH SHA256=… DOWNLOAD_ONLY YES)`, `contour::wasmtime` INTERFACE = SYSTEM include dir +
  static lib + `pthread dl m` (Linux) / `ws2_32 advapi32 userenv ntdll shell32 ole32 bcrypt` (Windows) and
  `LIBWASM_STATIC WASI_API_EXTERN=` defines on Windows (wasm.h/wasi.h otherwise `dllimport`). Else
  `find_package(wasmtime 48 QUIET)` accepting a `wasmtime::wasmtime` target. Else OFF with
  `Extension host engine: WAMR (<reason>)`. Gated on `CONTOUR_EXTENSIONS`. Wasmtime row added to the
  third-party summary.
- `CMakeLists.txt`: `set(CONTOUR_HAVE_WASMTIME OFF)` (plain variable, documented) next to
  `CONTOUR_WASM_RUNTIME`; `|> Extension host engine:` line in the configuration summary.

## Step 4 — Verification

Fresh configure `PACE_SCHEDULER=127.0.0.1:6674 cmake --preset clang-asan` → exit 0, then reconfigure
with `-DPACE_SCHEDULER=…` → exit 0, zero `CMake Warning`:

```
-- Using CPM to fetch Wasmtime 48.0.10 (x86_64-linux-c-api.tar.xz)
-- CPM: Adding package wasmtime@48.0.10 (48.0.10)
-- Extension host engine: Wasmtime 48.0.10
-- Wasmtime            CPM (Wasmtime 48.0.10, prebuilt, static)
-- |> Extension host engine:                          Wasmtime 48.0.10
$ ls out/clang-asan/_deps/wasmtime-src/lib/libwasmtime.a
out/clang-asan/_deps/wasmtime-src/lib/libwasmtime.a
```

Fallback branches exercised in a scratch project that includes the extracted block with a stub
`CPMAddPackage` (`/tmp/claude-1000/wasmtime-check/fallback/`):

```
FAKE_PROC=riscv64 CPM=ON  -> Extension host engine: WAMR (no pinned Wasmtime archive for Linux riscv64, and no Wasmtime 48 is installed) HAVE=OFF
FAKE_PROC=x86_64  CPM=OFF -> Extension host engine: WAMR (CONTOUR_USE_CPM is OFF and no Wasmtime 48 is installed) HAVE=OFF
FAKE_PROC=AMD64   CPM=ON  -> stub CPMAddPackage … x86_64-linux-c-api.tar.xz … ; Extension host engine: Wasmtime 48.0.10 HAVE=ON
```

`editorconfig-checker CMakeLists.txt cmake/ContourThirdParties.cmake` → clean. Nothing built (per brief);
no C++ changed, so no ctest run.

## Files changed

- `/home/christianparpart/projects/contour-extension-host/cmake/ContourThirdParties.cmake`
- `/home/christianparpart/projects/contour-extension-host/CMakeLists.txt`
- Local only: plan "### Phase-1 facts" section appended.

## Decisions / deviations

- `CONTOUR_HAVE_WASMTIME` is a normal variable initialised OFF in `CMakeLists.txt` (the "internal flag"),
  not a cache entry, so a stale cache cannot claim a Wasmtime that no longer resolves.
- Linux libs given verbatim (`pthread dl m`) rather than `Threads::Threads ${CMAKE_DL_LIBS}`.
- Windows static-link defines (`LIBWASM_STATIC`, `WASI_API_EXTERN=`) added; not in the brief but required
  by the headers.

## Concerns

1. macOS: no extra system libs/frameworks linked; whether Rust's std in `libwasmtime.a` needs e.g.
   `-framework CoreFoundation` is unverified until a macOS link (Task 1.3 / CI).
2. Windows link set and defines are unverified by an actual link (could be tried with the msvc-wine cross
   toolchain in Task 1.3).
3. The system-package path assumes a `wasmtime::wasmtime` imported target; upstream ships no CMake package
   config I could confirm, so in practice that path likely never resolves. The spec's "probe that checks
   version and features" is not implemented here (brief asked only for `find_package`).
4. Under ASan the prebuilt `libwasmtime.a` is uninstrumented; with signal-based traps it may conflict with
   ASan's SEGV handler — the spec already says `Software` traps in sanitizer builds.
