# ci4-windows — fix round 1 (review of 5446a7855)

Fold into 5446a7855 (fixup + autosquash); every commit builds; do not push. Constraints as in
task-5.8-ci4-windows-brief.md.

Important
1. PeImports_win32.cpp:~845 skips API-set imports, so on real Windows (msvc-ci Release, dynamic CRT)
   ucrtbase.dll never gets its own line and an init failure in it would be blamed on vcruntime140.dll.
   Keep API-set names in the closure, passed as bare names in dependency order; LoadLibraryExW resolves them
   through the API-set schema, so ucrtbase is reported on its own line. Under wine keep the test meaningful.
2. LoaderProbe.cpp:101-104: AllocConsole replaces the standard handles; say() re-reads
   GetStdHandle(STD_ERROR_HANDLE) per line, so after a successful AllocConsole the report may go to the new
   (then freed) console and the test loses its library=/end lines. Capture the report handle once, before
   AllocConsole. Avoid a visible console window on desktop runs if you can (e.g. check the ability to
   attach/create without showing, or free immediately and say so).

Minor
3. Report: replace the pseudoconsole-page rationale with the sharper, explicitly UNVERIFIED hypothesis:
   since Windows 8 conhost.exe is started as a child of the console client, and only the contained launch
   sets PROCESS_CREATION_CHILD_PROCESS_RESTRICTED (Process_win32.cpp:~471), so console setup at startup
   fails only contained. State that a green run proves DETACHED_PROCESS works, not why CREATE_NO_WINDOW
   failed. Put the same honest one-line rationale in the code comment at Process_win32.cpp:~540.
5. PeImports_win32.cpp:~847: .make_preferred() on beside-the-exe paths.
6. KnownDLLs: resolve names listed under HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs
   from System32 first (read the list, or a fixed table if reading is not possible in the test — say which).
7. LoaderProbe_test.cpp:~330: CHECK(!closure.libraries.empty()).
8. LoaderProbe_test.cpp:~301: make the failing assertion print the error code (string, not {?}).
9. Report: credit the StartupImports_test rationale rewording to the right commit.

Run: Windows cross build + wine for vtextension_process_test and vtextension_wamr_helper_test
"[appcontainer]"; clang-tidy/clang-format 22. Append to task-5.8-ci4-windows-report.md; return head, one-line
tests, concerns.
