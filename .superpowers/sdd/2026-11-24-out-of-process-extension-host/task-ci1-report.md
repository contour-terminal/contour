# CI fix 1: report

Status: DONE_WITH_CONCERNS (all local checks green; two CI jobs were still pending at the end, and the macOS
job had stopped early, so later macOS-only errors may still be hiding).

Old head: `05046ba58` → new head: `149c7edac`. Rebased onto `origin/master` `8c3afaa0c`. There are 146 commits,
as before, and no `fixup!` commit is left. Not pushed.

## Failures, causes, fixes

| # | Failure | Root cause | Fix | Folded into (old → new hash) |
|---|---------|------------|-----|------------------------------|
| 1 | Check C++ style: `DefaultBackend.cpp:4` | `WamrBackend.hpp` sat in the wrong include block for clang-format 22 | `/usr/bin/clang-format -i` (moves the include after `<utility>`) | `f6230198e` → `ab12b6f94` refactor: configure the extension runtime at every composition root |
| 2a | typos: `std::unexpect` read as "unexpected" (5 sites) | standard-library identifier missing from the dictionary | `_typos.toml`: `unexpect = "unexpect"` with a justification comment | `a4805b923` → `56be5b625` feat(vtextension): a Wasmtime backend that loads and describes modules (the first commit that used `std::unexpect`) |
| 2b | typos: `Line_test.cpp:142` "STATIC_CHECKs" read as `CHEC` | plural suffix on a macro name in a comment | reworded to "the static checks pin the helper" ("the STATIC_CHECK lines" would have pushed the line past clang-format's column limit) | `009b13c5e` → `234930b74` test(vtbackend): say which runtimes the multibyte trimming check bites on |
| 3 | ubuntu-24.04 (Clang 22 + libstdc++ 14): `HostileHost.cpp:706` | `std::ranges::contains(vector<unique_ptr>, nullptr)` needs `ranges::equal_to` over `unique_ptr&` and `nullptr_t&`, which GCC 14's libstdc++ rejects | `std::ranges::any_of(channels, [](auto const& channel) { return channel == nullptr; })` | `d9a624925` → `25597923e` test(vtextension): crash and hostility suite for the extension host process |
| 4 | Windows: `ProcessLauncher_test.cpp` hits `Sandbox.hpp(24) #error` | `ProcessLauncher_test.cpp` includes `HostBootstrap.hpp` (for `BuildCapPolicy`), which includes `Sandbox.hpp` (for `ConfinementFailure`). `vtextension_process_test` linked `vtextension_host`, which carries the PUBLIC `CONTOUR_PLATFORM_SANDBOX`, only under `VTEXTENSION_LINUX_SANDBOX`. So the test failed on every system without the Linux sandbox: Windows, macOS and FreeBSD | `vtextension_process_test` now links `vtextension_host` on every system, with a comment saying why. The Linux-only block keeps only its extra sources | `4cb8b2af7` → `4085f4c1e` feat(vtextension): the extension host restricts its system calls on Linux (it added the `#error` and the conditional link) |
| 5 | Ubuntu Linux 24.04 post-check | only aggregates #3 | none needed | n/a |
| 6 | **macOSArm** (failed during the run) | (a) the same `Sandbox.hpp` `#error` as #4, fixed by #4. (b) `GuestMemoryMapping_test.cpp:111`: `base` was used only in the `__linux__` and `_WIN32` branches, giving `-Werror,-Wunused-variable` on macOS | (b) `REQUIRE(base != nullptr);` right after `base` is declared: a real check on every system | `35343b1e4` → `0269b2c10` feat(vtextension): the extension host runs guests in memory Contour gives it |

### Sweeps the brief asked for
- **clang-format:** checked every C++ file the branch changes, and the whole of `./src/` exactly as CI checks
  it, with `/usr/bin/clang-format` 22.1.8. There are 0 violations.
- **`ranges::contains`/`find`/`count` against `nullptr` in the branch diff:** only the one at `HostileHost.cpp`
  was a match. The hits elsewhere in the tree (`SoftFont_test`, `SidebarQml_test`) are on master and outside
  this branch.
- **Includers of `Sandbox.hpp`:** I checked every target that compiles a file including it, directly or
  through `HostBootstrap`, `HostServer`, `HostProcess`, `InProcessHost` or `HelperComposition`. These targets
  link `vtextension_host` on every system: `extension_host_probe`, `vtextension_remote_test`,
  `vtextension_helper_test`, `vtextension_wamr_helper_test`, the `*_helper_engine` targets (PUBLIC) and
  `contour-extension-host[-hostile|-wamr]`. `vtextension_process_test` was the only one that did not.
- **Variables used only under a platform `#if`:** I ran a heuristic script over the branch's changed `.cpp`
  files, because the macOS job stopped at its first failures. It flagged nothing real beyond
  `GuestMemoryMapping_test.cpp`.

## Local checks (HEAD `149c7edac` unless noted)
- `find ./src ... | xargs /usr/bin/clang-format --Werror --dry-run`: 0 errors.
- `CHECK_SPELLING_REQUIRE_TOOL=1 python3 scripts/check-spelling.py` (typos v1.48.0, which the script
  fetches): `typos: spelling OK (32 exceptions, all justified).` The `--self-test` run reports 9 cases OK.
- `editorconfig-checker` on the touched files: clean.
- `HostileHost.cpp` compiled by hand with `-stdlib=libstdc++ -fsyntax-only` (only Fedora's libstdc++ 16 is
  installed, so GCC 14's behaviour was judged by reading): exit 0.
- **Windows cross build** (`out/clangcl-debug`, the `/opt/msvc` toolchain), building
  `vtextension_process_test` and `extension_host_probe`: exit 0. Under wine
  (`wine-run.sh bin/vtextension_process_test.exe`, own prefix): `test cases: 101 | 100 passed | 1 skipped;
  assertions: 799 | 799 passed`. The skip is `SharedObject_test.cpp:266`, which skips on purpose on that
  system.
- **Per-commit ASan builds** of every rewritten commit, building only the targets that commit's change
  touches. Each exited 0:
  - `ab12b6f94`: `vtextension_wamr`
  - `25597923e`: `contour-extension-host-hostile`
  - `0269b2c10`: `vtextension_process_test`
  - `234930b74`: `vtbackend_test`
  - `4085f4c1e`: `vtextension_process_test` and `extension_host_probe`
  - HEAD: every vtextension test target, `vtbackend_test`, `vtextension_wamr` and the three
    `contour-extension-host` executables, with 0 warnings
  - `56be5b625` changed `_typos.toml` only, so it needs no build.
- `ctest --preset=clang-asan -R vtextension`: 6/6 passed (`vtextension_test`, `vtextension_wasmtime_test`,
  `vtextension_process_test`, `vtextension_remote_test`, `vtextension_helper_test`,
  `vtextension_wamr_helper_test`).
- `vtbackend_test "[Line]"`: 249 assertions in 42 test cases passed.
- `vtextension_process_test "GuestMemoryMapping.opensOnlyTheInitialBytes"`: 13 assertions passed.

## CI jobs still pending (on the old pushed head)
- `Check clang-tidy (diff)`: still pending.
- `Ubuntu Linux (no status-line extensions)`: still pending.

## Deviations
- The `_typos.toml` entry went under `[default.extend-identifiers]`, not `[default.extend-words]`.
  `std::unexpect` is an identifier, and that section is where `countr_zero` and `msg_controllen` already sit.
  It is narrower, too: prose that misspells "unexpected" as "unexpect" is still caught. typos passes with it.
- macOSArm was not in the brief's list of failures but failed during the run. It is fixed in the same way
  (6 above).

## Concerns
- The macOS job stopped at its first failed objects (670/2086). Other macOS-only errors may surface once
  these are fixed. I can't build for macOS locally.
- The libstdc++ 14 fix is verified by reading only. No libstdc++ older than 16 is installed.
- clang-tidy (diff) has not reported yet, so its findings are unknown.
