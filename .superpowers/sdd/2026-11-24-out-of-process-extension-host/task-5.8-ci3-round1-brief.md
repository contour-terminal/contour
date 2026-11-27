# Task 5.8 ci3 — fix round 1

Review of head 2f64305cd: spec passes; quality has 1 Important + 4 Minor. Fix all five, folded into the
commits that introduced the code (fixup + autosquash), each commit building on its own. Do not push.

1. **Important — LNK4199 on MSVC** (`src/vtextension/wasm/remote/CMakeLists.txt:126`). The INTERFACE
   `/DELAYLOAD:ole32.dll`/`userenv.dll` reaches executables that never pull in `AppContainer_win32.obj` from
   the static library, where link.exe (CI's `msvc-ci` preset: cl + link.exe) warns "LNK4199: /DELAYLOAD
   ignored; no imports found". Zero-warning policy. Put the delay-load only on the executables that actually
   link the launcher's container code (contour-extension-host[-wamr], the probe, contour itself if it
   launches hosts, and the test binaries that use the launcher) — data-driven, e.g. one CMake function
   `contour_delay_load_container_libraries(<target>)` called per target — or find another way that both
   link.exe and lld-link accept warning-free. Explain in the report how you verified no LNK4199 arises
   (lld-link is silent; reason from which targets reference CoTaskMemFree/userenv symbols, e.g. by
   `llvm-nm`/`llvm-objdump` on the cross-built objects/executables).
2. **Minor — move the startup-import guard** out of `vtextension_process_test` (documented engine-free,
   CMakeLists.txt:152-153; the guard made it depend on contour-extension-host, :197-199) into
   `vtextension_helper_test` (:374-378), which already depends on host and probe. Also check
   `contour-extension-host-wamr` there when it is built.
3. **Minor — `LostExitStatus`** (ProcessLauncher.hpp:447-458): -1 now logs on POSIX as "exit code
   0xFFFFFFFF". Give it its own text ("its exit status was lost") as a row/case ahead of the hex formatting.
4. **Minor — `Row::same` bool** in AppContainer_test.cpp:242-247: make it an enum class (AGENT.md).
5. **Minor — diagnostic**: have the probe report its window station name (GetUserObjectInformationW on
   GetProcessWindowStation(), UOI_NAME) in the uncontained run's report, and have the test INFO it, so the
   next CI log confirms or refutes the service-window-station premise. It must not pull user32 into the
   contained probe's startup imports: delay-load or GetProcAddress it, used only in uncontained mode, and
   keep the import guard passing.

Constraints as in task-5.8-ci3-brief.md (unset CONTOUR_*, flock -o + nice 19 + ionice -c 3 + -j 4, one
tree, PID-only kills, no vendor/core-cpp edits, git commit -s with Signed-off-by Christian Parpart
<christian@parpart.family>, no AI trailers, no subagents, clang-format 22 at /usr/bin/clang-format,
user-facing text never says wasm/WAMR/Wasmtime).

Report to task-5.8-ci3-round1-report.md in this directory; return status, commits, one-line tests, concerns.
