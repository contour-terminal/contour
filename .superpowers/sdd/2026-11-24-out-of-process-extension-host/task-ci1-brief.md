# CI fix 1: make draft PR #2431 green where it is red

The branch `feature/out-of-process-extension-host` is pushed as draft PR #2431 at `5fdb8bfdf`. The local head is now `05046ba58` (task 5.3 was rebased after the push). CI run 45689033692/45689033605 reported the failures below. The cleaned logs are in `/tmp/claude-1000/-home-christianparpart-projects-contour/a3633faa-27fc-44d2-bb77-a2354e68aa02/scratchpad/ci/*.log`.

## Failures

1. **Check C++ style:** `./src/vtextension/wasm/wamr/DefaultBackend.cpp:4:1: error: code should be clang-formatted`. Introduced by `f6230198e`. CI uses clang-format 22, which is `/usr/bin/clang-format` here; never the wasi-sdk one. Then run `/usr/bin/clang-format --dry-run --Werror` over **every** C++ file the branch changes (`git diff --name-only origin/master...HEAD`), not only this one.
2. **Check spelling (typos):**
   - `std::unexpect` is flagged as "unexpected" at `SharedObject_test.cpp:92,103`, `SharedObjectName.hpp:53` and `CopyPathMemory_test.cpp:268,279`. Fix it in `_typos.toml`: add `unexpect = "unexpect"` under `[default.extend-words]`, which is the standard-library identifier. Do not rename the code.
   - `src/vtbackend/grid/Line_test.cpp:142`: the comment "STATIC_CHECKs" is flagged as `CHEC`. Reword it, for example "the STATIC_CHECK lines".
   - If `typos` is installed locally, run it over the repository. If not, find the CI's version in `.github/workflows` and install that exact binary into the scratchpad.
3. **ubuntu-24.04 (Clang 22 with GCC 14's libstdc++):** `src/contour-extension-host/testing/HostileHost.cpp:706`, `std::ranges::contains(channels, nullptr)` on a `std::vector<std::unique_ptr<RelayedChannel>>`. libstdc++'s `ranges::equal_to` cannot compare `unique_ptr&` with `nullptr_t&`. Use a projection (`&std::unique_ptr<RelayedChannel>::get`) or `std::ranges::any_of` with a predicate. Then grep the whole branch diff for other `ranges::contains`/`ranges::find` calls on containers of smart pointers compared against `nullptr`, and fix those too. If you can, configure a scratch build with `-stdlib=libstdc++` (Fedora's g++ libstdc++ 16 may accept code that 14 rejects, so judge by reading where needed). Do not create a full second tree: compiling the one translation unit by hand from `compile_commands.json`, with `-stdlib=libstdc++`, is enough.
4. **Windows (MSVC):** compiling `src/vtextension/wasm/remote/process/ProcessLauncher_test.cpp` for `vtextension_process_test` fails with `src/vtextension/wasm/host/Sandbox.hpp(24): fatal error C1189: #error: "CONTOUR_PLATFORM_SANDBOX is set by linking vtextension_host."`.
   - The target includes `Sandbox.hpp` (directly or indirectly) on Windows without linking `vtextension_host`, or the definition is not PUBLIC there.
   - Find the cleanest fix: link the target properly, move the definition to the interface the header belongs to, or stop including the header where it is not needed.
   - Check every target that can include `Sandbox.hpp` on every platform, not just this one.
   - Verify with the wine cross toolchain (`/opt/msvc`, tree `out/clangcl-debug`): build `vtextension_process_test` and run the relevant cases under wine.
5. **Ubuntu Linux 24.04 post-check:** this only aggregates the CC matrix. It goes green when 3 is fixed.

Before you finish, run `gh pr checks 2431` again. If `macOSArm`, `Check clang-tidy (diff)` or `Ubuntu Linux (no status-line extensions)` have failed by then, read their logs (`gh api --allow-escape-sequences repos/contour-terminal/contour/actions/jobs/<id>/logs`), and fix them too, the same way. If they are still pending, list them as such in the report.

## How to land the fixes (history matters: the owner merges with merge commits)

- **Fold each fix into the commit that introduced the defect:** `git commit --fixup=<that commit>`, then one `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash origin/master`. Run `git fetch origin master` first; this also rebases the branch onto the latest master. Resolve conflicts if any; strip all conflict markers, including diff3's `|||||||`.
- **A `_typos.toml` change** belongs with the first commit that used `std::unexpect`.
- **Afterwards, every commit must still build:** rebuilding each one is too costly. At minimum, build the commits whose content changed in the ASan tree, plus HEAD. Judge by exit status, never by grepping output.
- **Do NOT push.** The controller pushes.

## Rules

Read `run-rules.md` in this directory and follow it.
- Gentle limits: one tree at a time, `flock -o … nice -n 19 ionice -c 3 … -j 4`.
- In every shell, `unset $(env | grep -o '^CONTOUR_[A-Z_]*')` first.
- `git commit -s`, no trailers.

After the rebase, run `ctest --preset=clang-asan -R vtextension` once.

## Report

Write to `task-ci1-report.md`:
- each failure: its root cause, the fix, and which commit it was folded into;
- the old head → the new head;
- the local checks you ran, with their output;
- the CI jobs still pending.
