# Task 5.8 — CI round 5, Windows

Worktree /home/christianparpart/projects/contour-extension-host, branch feature/out-of-process-extension-host.
CI run 45826489690 ran on a954a9112. Local HEAD is c66b53a20, which adds macOS-only round-3 minors on top and is NOT pushed.
Full Windows log: /tmp/claude-1000/-home-christianparpart-projects-contour/a3633faa-27fc-44d2-bb77-a2354e68aa02/scratchpad/ci/Windows-a954a.log (vtextension_remote_test near line 6543-7412, vtextension_process_test near 7416-7800).

## Progress
DETACHED_PROCESS worked: the contained probe now starts and runs, and the 0xC0000142 is gone. The loader probe lines are all ok and console=ok.
Keep the LoaderProbe/StartupImports guard as it is; it is the regression test for that.

## Failures to fix (find the root cause of each before changing anything; use superpowers:systematic-debugging)

1. **Sandbox.windowsHelperRunsContained, container folder** (AppContainer_test.cpp:730).
   The probe reports `C:\Users\runneradmin\AppData\Local\Packages\contour.extensionhost\AC\` (Temp beneath it for TEMP and TMP).
   That is what it should report. The expected folder, however, is printed as the relative `Packages\contour.extensionhost\AC`, so
   expectedContainerFolder() (or the way it is built) is wrong in the test process. Fix the expectation so it is the absolute folder as the
   system names it. Do not loosen the comparison.
2. **Same case, no `spawn` line** (AppContainer_test.cpp:789).
   connect= was refused:connect:10060 (WSAETIMEDOUT) and the case took 21 s, so the probe most likely spent its time in a blocking connect
   and the report ended, or the probe was cut off, before the spawn row. Confirm this from the probe and the test's report reading.
   Then make the probe's connect attempt bounded (for example a non-blocking connect with a short wait, reported as its own refusal
   text, and kept in ContainedConnectRefusals) so that every row is always reported. A missing row must keep failing loudly.
3. **HostProcess.locksDownInTheSandboxOrder and HostProcess.hardensBeforeAnyThreadStarts** (HostProcess_test.cpp:599, also hit by five
   more cases through the same helper). On Windows, StopReceiver and StopSender report the SAME handle.
   Decide from the code whether that is correct on Windows (for example, one event serving both roles) or a bug (a handle recorded twice,
   or a missing second handle). If it is correct, the held-descriptor list must not record one handle twice; make the contract and the test
   say what Windows holds. If it is a bug, fix it.
4. **RemoteBackend.instancesHoldNoDescriptor** (RemoteBackend_test.cpp:2395): openHandleCount() was 124 against 123 before.
   Find which handle outlives the instances on Windows: a real leak, or a lazily created handle that is process-wide and created once,
   which the test must take before its baseline. Fix the leak, or make the baseline honest. Do not just widen the tolerance.

Previously these may have been hidden by msvc-ci's stopOnFailure. Check whether anything after them in vtextension_remote_test or
vtextension_process_test can be run locally under wine to surface further Windows-only failures now (the cross toolchain is in
/opt/msvc with clang-cl and wine). Report what you could and could not run.

## Constraints (binding)
- `unset $(env | grep -o '^CONTOUR_[A-Z_]*')` in every shell.
- Builds: `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target …`.
  Run the Windows cross-build under the same flock, nice, ionice and -j4. Use one tree at a time.
- Never kill processes by name or pattern, only by recorded PIDs.
- Never edit vendor/core-cpp. Never commit docs/superpowers or .superpowers.
- Commits: `git commit -s`, Signed-off-by: Christian Parpart <christian@parpart.family>, no AI trailers. Fold each fix into the commit that
  introduced the code (fixup + autosquash, without touching c66b53a20's content). Each commit must build on its own.
- Do NOT push. Do not dispatch subagents.
- User-facing text never says wasm, WAMR or Wasmtime.
- Use clang-format 22 at /usr/bin/clang-format.
- AGENT.md rules apply: enum class over bool, DI through the API table, data-driven tables, -Werror clean, Doxygen on new public declarations.

## Report
Write the full report to /home/christianparpart/projects/contour-extension-host/.superpowers/sdd/2026-11-24-out-of-process-extension-host/task-5.8-ci5-windows-report.md: the root cause of each failure with evidence, the fix, files, commits, and tests run
(Linux ASan targets, plus the Windows cross-build with wine for the affected binaries). Return only status, commits, a one-line test summary and concerns.
