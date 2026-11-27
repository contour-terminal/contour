# 5.8 CI round 2 — review round 1

Review of b5732de14: items ✅, one Important, four minors.

1. **Important (ruling: change the design).** Contained launches must never carry the parent's LOCALAPPDATA/TEMP/TMP values. Set all three to the AppContainer's own folder from `GetAppContainerFolderPath(sid)` (TEMP and TMP to its `Temp` subfolder if Windows' own convention says so — check the docs and state which), computed in the parent, independent of the parent's environment. This also closes minor 2 (a parent without these variables). Failure to get the folder path is a launch refusal at its own `LaunchStep` with the Win32 code, logged by the existing refusal table. The contained probe (`testing/LaunchProbe.cpp contained()`) reports, for each of the three, whether its value lies under the container folder; `AppContainer_test` CHECKs it, and that no value names the parent's profile directory. Under wine, run what wine supports and state what it skips.
2. **Minor 1:** move the shared `Wine.hpp` test helper and its migration out of 77695a0c7 into its own `test(vtextension)` commit placed before its first user.
3. **Minor 3:** fix the `Process_win32.cpp` file-head comment ("the environment it is given and nothing else") for the contained case.
4. **Minor 4:** document in `processVariable` that an empty value counts as absent (or drop the helper if item 1 removes its last user).

Base b5732de14 (pushed). Verify: Windows cross build + wine of process_test and the AppContainer set, Linux ASan vtextension suites, lints, clang-tidy diff, clang-format 22. Report: task-5.8-ci2-round1-report.md. Same constraints as before.
