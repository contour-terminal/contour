# Task 5.8 — CI round 4, macOS: first full run of the Seatbelt sandbox fails

Where this fits: Contour's out-of-process extension host (`contour-extension-host`, one helper process per
extension) is locked down per OS — Linux seccomp+Landlock, FreeBSD Capsicum (green on CI), Windows
AppContainer (separate round in progress), macOS Seatbelt. macOS CI (macOSArm job, preset `macos-ci`,
`-DCONTOUR_EXTENSION_HOST=out-of-process -DCONTOUR_WASM_RUNTIME=wasmtime`) ran the sandbox tests for the
first time on commit c2d8d12e5 (run 45790815170) and failed. There is no local Mac: diagnose from the log,
the code and Apple's documentation; each wrong guess costs a CI round, so any fix you are not sure of must
ride with a diagnostic that settles it on the next run.

Base: local head ce9062c11 (do not push). Full log:
/tmp/claude-1000/-home-christianparpart-projects-contour/a3633faa-27fc-44d2-bb77-a2354e68aa02/scratchpad/ci/macOSArm-c2d8d.log
Find the sandbox-lockdown code with `git grep -n -i seatbelt -- src cmake .github` and the macOS
`Sandbox_*`, `Process_macos.cpp`, `SandboxProbe*` files under src/vtextension/wasm/remote/process/.
The ledger's Task 5.6 entries (`.superpowers/sdd/2026-11-24-out-of-process-extension-host/progress.md`,
grep "Task 5.6") record the macOS design decisions and parked minors.

## Failures (all must be explained; fix each at its root)

1. `vtextension_process_test` — `Sandbox.forbiddenOperationsFailFromAnExecutor`
   (SandboxProbe_test.cpp:283/299), log ~7620-7670:
   - `socket-inet`: "ok", expected "refused:1" (EPERM)
   - `socket-unix`: "ok", expected "refused:1"
   - `execve`: "refused:2" (ENOENT), expected "refused:1"
   - `protect-writable-executable`: refused:13 (warning only).
   Sockets being allowed means the lockdown is either not applied to the executor thread/process the probe
   runs the operation from, or the profile permits them. Decide which, from code + docs. If socket creation
   is genuinely not a Seatbelt-deniable operation in the form the probe uses, the expectation table changes
   only with a stated reason; do not loosen a refusal to make a test pass.
2. `vtextension_test` — `PduStream.lanesNeverInstallADescriptor` (PduStream_test.cpp:487): open descriptor
   count 6 vs 5 before. Something on macOS installs a descriptor (a lane's wake primitive? kqueue? a
   lazily created one-time descriptor by the OS/libc on first use?). Find it; if it is a one-time lazy OS
   descriptor, the test must warm it up first, with the reason in a comment — otherwise fix the leak.
3. `vtextension_test` — whole binary timed out at 600 s (ctest Timeout). Identify which case hangs (the
   `[trace] case:` / `ended:` lines; the binary's output is near the "Test #43: vtextension_test" line —
   look for a case that started and never ended) and why on macOS only.
4. `vtextension_wamr_helper_test` (a later step that exited 42): 34 of 62 cases fail, mostly
   `BackendContract.hpp:613 REQUIRE(instance.has_value())`, `instantiatesConcurrently` 0/25,
   `Bundled: git polls…` 0 commands. The WAMR helper (`contour-extension-host-wamr`) apparently cannot
   instantiate under the macOS lockdown (or at all) — find the refusal reason (the instance error, the
   helper's stderr, the lockdown step) and fix the cause. Note the Wasmtime helper gets the JIT entitlement
   and the WAMR helper does not (by design: WAMR interprets), so a profile/entitlement mismatch is a lead.

Also check the "verify … actually ran" steps of the macOS job for anything relying on these suites.

## Requirements

- Root causes with evidence in the report (log line numbers, code lines, docs).
- Fixes in production code where the defect is production; tests changed only where the test was wrong,
  with the reason.
- Where certainty is not possible without a Mac, add a diagnostic that makes the next CI log decide it
  (e.g. the probe reports which lockdown step ran and the sandbox profile's applied state; the helper's
  refusal text reaches the test's failure message).
- Linux ASan of the touched targets green; clang-tidy and clang-format 22 clean; macOS-only code must at
  least be syntax-checked as far as possible locally (state how, or that it was not).

## Constraints

unset $(env | grep -o '^CONTOUR_[A-Z_]*') in every shell. Builds:
`flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target …`
— one tree. PID-only kills. Never edit vendor/core-cpp. Never commit docs/superpowers or .superpowers.
`git commit -s`, Signed-off-by: Christian Parpart <christian@parpart.family>, no AI trailers. Fold fixes into
the commit that introduced the defect when that is clear (fixup + autosquash), else a new commit; every
commit builds on its own. Do not push. No subagents. clang-format 22 at /usr/bin/clang-format.
User-facing text never says wasm, WAMR or Wasmtime. AGENT.md rules (enum class over bool, DI, data-driven
tables, -Werror, Doxygen on new public declarations, no NOLINT).

Report to task-5.8-ci4-macos-report.md in this directory; return status, commits, one-line tests, concerns.
