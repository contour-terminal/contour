# Task 5.8 — CI round 4, Windows: 0xC0000142 persists; the window-station hypothesis is refuted

Base: local head 42aeb492b (reviewed, unpushed). CI ran c2d8d12e5 (run 45790815170). Log:
/tmp/claude-1000/-home-christianparpart-projects-contour/a3633faa-27fc-44d2-bb77-a2354e68aa02/scratchpad/ci/Windows-c2d8d.log
(failure ~6655-6663; the [appcontainer] verify step ~7500-7690).

Facts from that log:
- The uncontained probe reports `window-station=WinSta0`, container=no, everything else as expected.
- The contained probe still ends `exit code 0xC0000142` before main, nothing on stderr — with ole32/userenv
  no longer imported anywhere (run-time lookup). So ole32→user32 was not the (only) cause, and the runner
  is not in a service window station.
- Our new exit text claims the cause: "a library it loads failed to initialise … a window station the app
  container may not use". That text asserts a refuted cause — fix it to say only what the status means.

## Item 1 — make CI name the failing DLL (diagnostic first, in this same push)

Wine cannot reproduce; each guess costs a CI round. So this push must carry a diagnostic that names the
cause outright. Proposal (adapt if you find a better one):
- A tiny stub executable (imports kernel32 only; verify with the import reader you already have) launched
  through the same production contained launch. If it starts, the container itself is fine and the failure
  is a DLL in the probe's/host's load graph.
- The stub, given a list of DLL names/paths on its command line (the probe's and host's import closure,
  computed by the test from the PE import tables recursively, resolving next-to-exe first, then System32),
  calls LoadLibraryExW on each, in dependency order, and reports `dll=<name> ok|error:<GetLastError>`
  over the same report channel the probe uses. ERROR_DLL_INIT_FAILED (1114), ERROR_ACCESS_DENIED (5) and
  ERROR_MOD_NOT_FOUND (126) then name the culprit.
- The test INFOs every line so the CI log shows it, and fails with the first failing DLL named.

## Item 2 — the strongest remaining hypothesis to check with it (and fix if confirmed)

The build directory D:\a\contour\contour\out\build\msvc-ci\bin is not readable by ALL APPLICATION
PACKAGES, whereas an installed Contour (Program Files) is. The exe image itself may map fine (created by the
parent), but DLLs beside it (MSVC runtime if app-local, vcpkg DLLs such as fmt/yaml/etc., Qt if any) are
loaded under the container token. Check which non-system DLLs the probe and contour-extension-host(-wamr)
depend on in the msvc-ci build (CMake: vcpkg triplet dynamic? VCPKG_APPLOCAL_DEPS?).
- If confirmed: decide the production answer explicitly (installed layout under Program Files is readable;
  a portable/zip install elsewhere may not be — the launcher could grant the container SID read+execute on
  the helper's own directory at provision time, or the host can be linked statically against those DLLs;
  prefer whatever keeps production correct for every supported install layout, and say which layouts you
  verified) and make the test environment match it.
- Other candidates to keep in mind: a DLL whose DllMain touches the registry/profile (HKCU) the container
  cannot open; the CRT reading env vars.

## Item 3 — truthful text

The exit-code table text for 0xC0000142 must state the meaning only ("a library it loads failed to
initialise"), no claimed cause. Same for any other row that guesses.

## Constraints

As in task-5.8-ci3-brief.md (unset CONTOUR_*; flock -o + nice -n 19 + ionice -c 3 + -j 4; one tree;
PID-only kills; never edit vendor/core-cpp; git commit -s, Signed-off-by: Christian Parpart
<christian@parpart.family>, no AI trailers; fold fixes into the commits they fix, each commit builds; do not
push; no subagents; clang-format 22 at /usr/bin/clang-format; user-facing text never says wasm/WAMR/Wasmtime;
AGENT.md rules). A diagnostic test that stays in the tree must be a proper test (named, data-driven,
asserting), not a printf.

Report to task-5.8-ci4-windows-report.md; return status, commits, one-line tests, concerns.
