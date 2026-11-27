# CI fix 3: Windows duplicate engine symbols; interpreter helper dies in a debug build

The branch is draft PR #2431. The local HEAD is `5798ba3f6`, which is not pushed yet. CI ran on the pushed head `9fb129082`. Cleaned logs are in `/tmp/claude-1000/-home-christianparpart-projects-contour/a3633faa-27fc-44d2-bb77-a2354e68aa02/scratchpad/ci/`: `r3-Windows.log` and `r3-ubuntu-24.04.log`.

## 1. Windows (MSVC): duplicate symbols when one test links both engines
`bin/vtextension_remote_test.exe` fails to link (`r3-Windows.log` ~5961):

```
wasmtime.lib(wasmtime_c_api-…) : error LNK2005: wasm_byte_vec_copy already defined in iwasm.lib(wasm_c_api.c.obj)
… wasm_byte_vec_new / _new_empty / _new_uninitialized …
```

Both Wasmtime and WAMR export the standard `wasm.h` C API. ELF linkers take the first definition, but MSVC refuses. Since the change that made Linux run extensions out of process by default, several test binaries link `vtextension_wamr` by name, and `vtextension_remote_test` also links Wasmtime. On Windows x86_64 Wasmtime is present.

- Find every target that can end up linking both `iwasm` and `wasmtime`, on any platform.
- Fix it cleanly. Never with `/FORCE`, and never by suppressing LNK2005. Options:
  - split the in-process WAMR composition of the remote contract out of `vtextension_remote_test` into its own test binary;
  - drop one engine from a binary that does not need it.
- Choose the cleanest option and keep the test coverage.
- Check by reading the CMake on all platforms. If the wine/clang-cl tree can reproduce the LNK2005 (lld-link with `/WX`? Probably not; lld may accept it), say so.
- Also check the `scripts/check-wasm-runtime-containment.py` rules. That lint must stay green, and you may need to extend it so that one binary linking both engines is a lint error. That would be ideal, since it would catch this on Linux too.

## 2. ubuntu-24.04 (`gcc-debug` preset compiled with clang-22, non-sanitized Debug): the WAMR helper dies on `extended-const`
In `r3-ubuntu-24.04.log` ~7908, `vtextension_wamr_helper_test` fails:
- the case is `WasmContract.acceptsEnabledFeatures - ProcessTraits`, section `extended-const`;
- `loaded.has_value()` is false;
- the run gate reports "1 extension host(s) ended uncleanly".

The helper's own report does not appear in the CTest output. The ASan and release trees pass locally. This configuration is a plain Debug build with no sanitizer, so its seccomp row set is the release/plain one, while WAMR is built Debug (assertions, maybe different code paths). The likeliest cause is a system call or an argument on the extended-const load path that the interpreted rows do not allow, which ends in SIGSYS. It may also be a WAMR assertion or abort.

- Reproduce it locally. You need a non-sanitized Debug tree; check what presets exist (`cmake --list-presets`). Configure ONE extra tree, for example `clang-debug` or a `gcc-debug`-equivalent, with `PACE_SCHEDULER=127.0.0.1:6674`. Build only `contour-extension-host vtextension_wamr_helper_test` there, niced at `-j 4`. Run the failing case. Use `strace -f` or the helper's own report to find what kills it.
- Fix the root cause: a row with a reason, or a code fix. Then make sure the helper's report reaches the test output whenever a helper ends uncleanly, so the next CI failure of this kind explains itself.
- Delete the extra tree when done, or keep it and say so. Only one heavy build may run at a time.

## 3. macOSArm
It may still be running for `9fb129082`. Before you finish, run `gh pr checks 2431`. If macOSArm has failed, read its log with `gh api --allow-escape-sequences repos/contour-terminal/contour/actions/jobs/<id>/logs`. Its deny-log and entitlement steps print diagnostics. Fix what it shows, the same way. If it is still pending, say so.

## How to land
- Fold each fix into the commit that introduced the defect (`git commit --fixup` + `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash origin/master` after `git fetch`).
- For a new lint rule or a split test binary that has no natural owner, use a separate commit.
- Each commit must still build.
- Do NOT push.

## Rules
`run-rules.md` applies:
- gentle limits;
- `unset $(env | grep -o '^CONTOUR_[A-Z_]*')` in every shell;
- `git commit -s`, with no trailers.

Before reporting, run: `ctest -R 'vtextension|wasm_runtime_containment'`, `-L lint`, CI's tidy-diff, clang-format, typos and editorconfig.

## Report
Write `task-ci3-report.md` with, for each failure: the root cause with evidence, the fix, and where it was folded. Also give the old head → new head, the local checks with their output, and what CI must confirm.
