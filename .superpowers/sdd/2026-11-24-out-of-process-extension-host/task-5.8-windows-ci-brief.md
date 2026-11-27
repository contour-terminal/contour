# 5.8 — first real Windows CI run: two failures

Windows CI on c130dab7c (run 45738705740) is the first Windows run that got past linking (earlier ones hit LNK2005, fixed since). Log: /tmp/claude-1000/-home-christianparpart-projects-contour/a3633faa-27fc-44d2-bb77-a2354e68aa02/scratchpad/ci/windows-c130.log (escape sequences stripped). Wine passes both, so wine is not evidence here: diagnose from the code and Microsoft docs (Microsoft Learn), then make the fix provable on wine where possible and state what only real Windows can prove.

1. `vtextension_wasmtime_test` — `WasmtimeEngine.signalTrapsPastTheOpenBytes` (WasmtimeEngine_test.cpp:949): `REQUIRE_FALSE(called.has_value())` "past the open bytes" — a guest access past the opened bytes did not trap on Windows. Likely suspects: Windows commit/reserve granularity (64 KiB allocation granularity vs 4 KiB page) in the guest memory provisioning (MemoryProvision / GuestMemoryMapping_win32), VirtualAlloc MEM_COMMIT rounding, or the mapping view size. Find the cause; fix the product code if the guard is really missing (that is a sandbox-relevant memory bound, treat as Important), or fix the test only if the test's expectation is wrong for Windows and the bound still holds — justify in the report.
2. `vtextension_process_test` — `AppContainer` case at AppContainer_test.cpp:552 (`probeReportIn(...)` with contained isolation): launch returns "the system refused". Preceding NotGranted/package-grant cases pass. Likely suspects: the build tree's probe directory lacks the ALL APPLICATION PACKAGES RX grant on the runner (test must grant/ensure it like the product does, or skip with a stated reason when the user may not grant), a dependency DLL (wasmtime.dll, MSVC runtime, vcpkg bin dir) outside a granted directory, or CHILD_PROCESS_POLICY / mitigation flags refused on that Windows build. Map the refusal to its Win32 error (log GetLastError in launchErrorText's detail if it is lost) so the next CI run names it.

Then: a cross build (/opt/msvc, clang-cl, lld-link) and wine run of both suites. Fold fixes into owning commits. Report: task-5.8-windows-ci-report.md.

## Also (round-3 review minors, ruled in)

3. `.gitattributes`: add `cmake/patches/*.patch -text` so the patch stays LF on Windows checkouts.
4. `cmake/ApplyPatchOnceTest.cmake`: add a fourth case where the tree is its own git clone (`git init` + commit in `_tree`), the production path for a CPM-fetched WAMR.
5. Build the `_WIN32` halves of the out-of-memory classifier table and its tests in the cross build and run them under wine.

Base: 9aefa2abe (pushed). Constraints: one build tree at a time, `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 … -j 4` for every build (the Windows cross build included), unset CONTOUR_* in every shell, kill only recorded PIDs, no subagents, `git commit -s`, no AI trailers, never push, never edit vendor/core-cpp; fold into owning commits with fixup + autosquash where clean.
