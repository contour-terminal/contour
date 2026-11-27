# Task 5.8 fix wave — review round 2 report

Base `4491235ab` → **head `790ce3259`** (local, not pushed; base `origin/master` = `8c3afaa0c`). All changes folded
into their owning commits; autosquash clean, tree equal to the tested one. Only the head commit's message was
amended (to say what the engines now map).

| Item | Folded into |
|---|---|
| 1, 2 | `790ce3259` fix(vtextension): a host short of memory as it starts is retried, and a lacking sandbox is a warning |
| 4, 5 | `03c755b81` fix(vtextension): unlink a WAMR execution environment under its thread cluster's lock |
| 3 (ruling) | "the extension host locks down in a fixed order, its threads prepared first" |

## 1. Priming out-of-memory is reachable (Important)

- **Wasmtime** (`WasmtimeEngine.cpp`): `resourceErrorOf(std::string_view message)` (public in
  `WasmtimeEngine.hpp`) reads a Wasmtime error message against a table, `ResourceErrorMarkers`: ENOMEM's text and
  `os error 12)`; Windows' `ERROR_NOT_ENOUGH_MEMORY`, `ERROR_OUTOFMEMORY` and `ERROR_COMMITMENT_LIMIT` texts and
  numbers (8, 14, 1455); and "out of memory". Matching messages become `OutOfMemory`, all others `RuntimeUnavailable`.
  `compilePriming` (module compile) and `runPriming` (instantiate, call) map their `wasmtime_error_t` through it.
  These stay `RuntimeUnavailable`:
  - a null engine: Wasmtime aborts rather than return null on an allocation failure, so null means a refused
    configuration;
  - a null store, same reason;
  - traps;
  - a missing export.
- **WAMR** (`WamrThreadEnv.cpp`): `ThreadEnv::enter` now tells "no runtime of this generation is up"
  (`RuntimeUnavailable`) from "`wasm_runtime_init_thread_env()` failed" (`OutOfMemory`).
  - The only failures behind the latter are allocations. On Windows, `os_thread_env_init` makes the thread's record
    with `BH_MALLOC`, plus a semaphore, a mutex and a condition (`win_thread.c:414-440`).
  - On POSIX, with hardware bounds checks off, it cannot fail (`wasm_runtime_common.c:1842-1863`).
  - `WamrThreadEnv.hpp`'s doc is updated.
- **`WamrThreadEnv.cpp:77` (`wasm_runtime_init`) is deliberately left `RuntimeUnavailable`.**
  - It is not on the priming path: `prepareThread` only calls `ThreadEnv::enter`.
  - It is reached by `createRuntime`, where `ExtensionError::OutOfMemory`'s user text ("extension exceeded its
    memory limit") would blame the extension.
- **End-to-end test** `RemoteBackend.hostShortOfMemoryAsItPrimesIsRetried` (remote_test):
  - A real `HostServer` runs in this process (`InProcessHostConnector`) on a Wasmtime hosted engine. The engine is
    wrapped in `PreparationHookEngine`, whose `prepareThread()` returns `OutOfMemory`; that is the engine's own seam.
  - The server sends its Failed Hello at Priming, and `HelperIncarnation` refuses it as `OutOfResources`.
  - A recording connector sees two refusals over two loads: the refusal is transient, so each load starts a host
    again. Both loads fail `HostUnavailable`. Passed (7 assertions).
- **Classifier test** `WasmtimeEngine.resourceErrorsAreOutOfMemory` (wasmtime_test): three resource messages map to
  `OutOfMemory`; a parse error, EACCES and an empty message map to `RuntimeUnavailable`. Passed (6).
- Not covered: Wasmtime raising a real out-of-memory error during priming (it cannot be provoked portably), and WAMR's
  Windows allocation failure.

## 2. `vtextension.warning` at namespace scope
`warningLog` is now defined in `HostRefusals.cpp`'s anonymous namespace, as `connectionLog` is, and enabled
by default. It is therefore registered at static initialisation, before the startup filter applies, so it can be
listed and turned off. The header's doc ("on unless turned off") now holds.

## 3. ExecutorThread test (ruling)
Renamed to `ExecutorThread.startedThreadRunsTheBody`, with its comment rewritten to say what it checks: a thread
that `start()` returned runs its body, waited for with a 10 s bound. No new assertion.

## 4. WAMR patch applied once, keyed by its content
- New `cmake/ApplyPatchOnce.cmake`:
  - It runs `patch -p1 -R --dry-run`; if that succeeds, the whole patch is already in place: it says so and does
    nothing.
  - Otherwise it runs `patch -p1 -N --forward`. That refuses (rather than reverses) a hunk it finds applied, so a
    half-patched tree fails loudly.
- The WAMR fetch now uses it as its `PATCH_COMMAND`, instead of CPM's `PATCHES` (plain `patch -p1 <`, not
  idempotent).
- `CUSTOM_CACHE_KEY "25bd7eb63e82-<first 12 hex of the patch's SHA-256>"` keys the CPM source cache by the patch's
  content (CPM.cmake:875 uses it as the download directory).
- The patch program is found on PATH. On a Windows host without one, it is looked for beside Git for Windows, as
  CPM's own `PATCHES` does. If none is found, configure stops with a clear error.
- Proof:
  - Two reconfigures of the ASan tree over its already-patched `_deps/wamr-src`: both exit 0. The patch step re-ran
    (stamp re-written), and the source diff is unchanged (the patch is in once, not twice).
  - The script run directly on that tree prints "Already applied" and exits 0.
  - On a fresh copy of the thread-manager sources it applies ("patching file …", exit 0); a second run says
    "Already applied" and exits 0.
- Not tested: a populated `CPM_SOURCE_CACHE`. The tree here has none; the key change is by construction.

## 5. Configure summary
The `WAMR` line now ends:

| How WAMR was obtained | Summary text |
|---|---|
| system package | "system package, without Contour's thread-safety patch (its interrupt verified)" |
| embedded | "embedded, without Contour's thread-safety patch" |
| CPM | "CPM (2.4.5, static, fast interpreter, with Contour's thread-safety patch)" |

The added text names no engine. I left the line's label `WAMR` as it was: it is the dependency summary's row name,
beside `Wasmtime` and `libunicode`. Renaming one row of that table would be inconsistent; say if all of them should
change.

## Verification

| Check | Result |
|---|---|
| ASan build, 11 vtextension targets | exit 0, 0 warnings |
| ASan suites (`~[.measure]`) | vtextension 486/26719; wasmtime 66 (63 + 3 skip)/1197; process 140 (139 + 1 skip)/5582; remote 302/12382; remote_wamr 55 (54 + 1 skip)/655; helper 161 (160 + 1 skip)/3916; wamr_helper 62/1237 |
| `ctest --preset=clang-asan -R 'vtextension\|wasm_runtime_containment'` | 9/9 |
| `ctest -L lint` | 34/34 |
| reconfigure over patched WAMR (item 4) | 2× exit 0, patch present once |
| `clang-tidy-diff.sh --base fw-pre` | 75 TUs clean |
| clang-format 22 `--dry-run -Werror` / editorconfig-checker | clean / clean |

TSan was not re-run this round: the brief asked for ASan. The only thread-relevant change is moving a static's
definition to namespace scope.

## Concerns
- The Wasmtime classification goes by message text: whatever Wasmtime and the OS put in the message. A message
  format change degrades it to `RuntimeUnavailable` (lasting), which is safe, not wrong.
- The WAMR mapping is reachable on Windows only, and is untested there: no cross build or wine run this round.
- Commits are still not built one at a time (deferred item 2).
