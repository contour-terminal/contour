# Task 5.8 ci3 — round 2 (review minors, while CI runs on c2d8d12e5)

c2d8d12e5 is pushed and passed re-review. Fix these minors, folded into the commits that introduced the
code (fixup + autosquash), each commit building on its own. Do not push.

1. **Lost exit status must not share a value with a real exit code** (ProcessLauncher.hpp:146, :171;
   users: PosixLaunch.cpp:231, Process_linux.cpp:81, ExtensionHostSupervisor.cpp:687, and any others).
   `LostExitStatus` is `{Exited, -1}`, so on Windows a host that really exits with 0xFFFFFFFF now logs
   "its exit status was lost". Add a distinct `ExitKind` enumerator for a lost status (name it after the
   meaning), make `LostExitStatus` use it, and route the text through the existing data-driven table.
   Check every `switch` on ExitKind (exhaustiveness) and every test that compares against LostExitStatus.
2. **Doc clause** at `ContainerFunctions` (AppContainer_win32.cpp:47-56): say userenv and combase are
   deliberately loaded once and never freed, which is what keeps the looked-up pointers valid.
3. **Correct your round-1 report's Concerns**: the startup-import guard DOES run on Windows CI. The
   msvc-ci test preset has `stopOnFailure: true`; run 785859 stopped at test 53 of 79 after
   vtextension_process_test failed, and the workflow's `[appcontainer]` verification step runs
   vtextension_wamr_helper_test, which includes the guard's `[appcontainer]` case. Edit the report file.

Not in scope (ruled): a test for the adapter's null-function checks — thin platform adapter, behaviour of
the refusal mapping is covered by the fakes.

Constraints as in task-5.8-ci3-brief.md. Report to task-5.8-ci3-round2-report.md; return status,
commits, one-line tests, concerns.
