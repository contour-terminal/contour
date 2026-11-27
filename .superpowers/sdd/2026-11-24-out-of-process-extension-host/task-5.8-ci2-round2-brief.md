# 5.8 CI round 2 — review round 2

Re-review of 785859fbf: items ✅, one Important, four minors. Fold into df749af5a (the contained-environment commit) where clean.

1. **Important:** the probe's `inside` verdict is a prefix test and accepts a doubled or swapped path. The parent computes the expected folder (`DeriveAppContainerSidFromAppContainerName` + `systemAppContainerApi().folderPath`) and the test CHECKs each `value-NAME` the probe reports for exact equality with `folder / row.subfolder` from the `ContainerVariables` rows. Remove the probe's own `GetAppContainerFolderPath` call (also closes minor 2).
3. `AppContainer_win32.cpp:321-324`: own the folder string with a `CoTaskMemFree` deleter before building the path.
4. `LaunchProbe.cpp:1005-1013`: `alignas(TOKEN_APPCONTAINER_INFORMATION)` on the buffer (if the code survives item 1).
5. `AppContainer_win32.cpp:359-367`: refuse an empty folder at `ContainerFolder`.

Verify: Windows cross build + wine (process_test, AppContainer set), Linux ASan vtextension suites, lints, clang-tidy diff, clang-format 22. Report: task-5.8-ci2-round2-report.md. Same constraints as before.
