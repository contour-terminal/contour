# Task 5.8 — CI round 3 (Windows contained launch: 0xC0000142) + two review minors

Worktree: /home/christianparpart/projects/contour-extension-host, branch feature/out-of-process-extension-host.
Local HEAD ee4d5ba28 (NOT pushed). CI ran on 785859fbf (= HEAD~0 minus ci2 round 2, which only made the
probe's container-folder check exact, freed the folder with CoTaskMemFree, refused an empty folder).

## CI result for 785859fbf (run 45771518903)

- ubuntu-24.04-arm Clang and GCC: green (sandbox tests pass on aarch64).
- FreeBSD: green.
- Windows: ONE failure, `Sandbox.windowsHelperRunsContained` (AppContainer_test.cpp:633 via :693):

```
contained
launch: started
no report: it exited with 0xC0000142; it wrote nothing to stderr
```

Full log: /tmp/claude-1000/-home-christianparpart-projects-contour/a3633faa-27fc-44d2-bb77-a2354e68aa02/scratchpad/ci/windows-785859.log
(failure near line 6753; the `[appcontainer]` re-run near 7622). Everything else on Windows passed,
including `AppContainer.launchCarriesTheIsolation` and `LaunchSpelling.helperEnvironmentContainedIsOneBlock`.

Progress versus the previous run: the CreateProcessW 203 is gone — the contained process now STARTS and dies
in loader initialisation: 0xC0000142 = STATUS_DLL_INIT_FAILED (some DLL's DllMain/initialisation failed
before main). The uncontained probe run in the same case works.

## Item 1 — diagnose and fix 0xC0000142 (the real work)

Use superpowers:systematic-debugging. Find the root cause before changing anything. Hypotheses to test, not
conclusions:

- **Window station / desktop access.** GitHub's Windows runner runs jobs as a service; the process's window
  station/desktop ACL may not admit the AppContainer SID, and user32.dll (pulled in directly or through
  another DLL — check the probe's and the helper's import tables, e.g. `llvm-objdump -p` / `llvm-readobj
  --coff-imports` on a cross-built probe, recursively for first-party and runtime DLLs) fails its init with
  exactly this status. This also matters for real users only if it reproduces in an interactive session;
  say which you believe and why. If this is it, the clean fix is the one Chromium's sandbox uses: the
  launcher gives the contained process its own desktop (and, if needed, window station) whose DACL grants
  the container SID, set via STARTUPINFO.lpDesktop — as one more data-driven LaunchStep with its own error
  code, owned RAII handles, through the injected API table so ContainerSandbox/AppContainer tests can drive
  it platform-free. Alternatively, if user32 is reachable only through something removable, removing the
  dependency is cleaner — judge which.
- **DLLs from the build directory** the container cannot read/initialise (runtime/debug CRT, sanitizer
  runtime, vcpkg DLLs next to the exe). Check what the Windows CI preset links and whether those DLLs sit in
  a directory whose ACL lacks ALL APPLICATION PACKAGES; note that an unreadable DLL usually gives
  0xC0000135/0xC0000022, a failing DllMain gives 0xC0000142.
- **The environment block** (SystemRoot, LOCALAPPDATA, TEMP, TMP only): a DLL needing another variable at
  init (e.g. `windir`, `SystemDrive`, `PATH`?) — check against what the uncontained run receives.

Whatever the cause, the production launcher (not just the test) must get a real extension host running
contained on such a machine. Wine will not reproduce this; reason from the Windows documentation (the
Microsoft Learn MCP tools are available) and the import tables, and make the failing case's message name
the cause if it ever recurs (e.g. the refusal/launch step that failed).

If the fix adds a LaunchStep: extend LaunchRefusal_win32's table, the step-spelling tests, and
AppContainer_test/ContainerSandbox tests for the new step's success and refusal paths.

## Item 2 — review minor: tolerant path compare (AppContainer_test.cpp ~708)

`checkContainerVariables` compares the probe's reported value to the expected folder byte for byte. Make it
compare as Windows compares paths: case-insensitively (`CompareStringOrdinal(…, TRUE)` on the wide strings)
and ignoring one trailing `\` on either side. Keep it exact otherwise (no prefix match).

## Item 3 — review minor: the value line must be present (AppContainer_test.cpp ~656-662, ~708)

`valueOf` returns "" when the `value-NAME=` line is missing, so "absent line" and "empty value" are
indistinguishable. CHECK that the line is present before comparing (return std::optional or a separate
presence check), so a probe that stopped reporting a variable fails instead of passing as "nothing".

## Constraints (binding)

- `unset $(env | grep -o '^CONTOUR_[A-Z_]*')` in every shell.
- Builds: `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target …`.
  Windows cross-build per the windows-cross-toolchain setup you used before (/opt/msvc, clang-cl, wine), same
  nice/ionice/-j4 under the same flock. One tree at a time.
- Never kill processes by name/pattern; only recorded PIDs.
- Never edit vendor/core-cpp. Never commit docs/superpowers or .superpowers.
- Commits: `git commit -s`, Signed-off-by: Christian Parpart <christian@parpart.family>, no AI trailers.
  Fold fixes into the commit that introduced the code where it is a fix of it (fixup + autosquash is fine);
  each commit must build on its own.
- Do NOT push. Do not dispatch subagents.
- User-facing text never says wasm, WAMR or Wasmtime.
- clang-format 22 at /usr/bin/clang-format.
- AGENT.md rules: enum class over bool, DI through the API table, data-driven step tables, -Werror clean,
  Doxygen on new public declarations.

## Report

Write the full report to `.superpowers/sdd/2026-11-24-out-of-process-extension-host/task-5.8-ci3-report.md`:
root cause with evidence, the fix, files, commits, tests run (Linux ASan target(s), Windows cross-build +
wine for the affected binaries), and whether this affects interactive desktops. Return only status, commits,
a one-line test summary and concerns.
