# 5.8 fix wave — review round 2

Re-review of round 1 (head 4491235ab): all 8 items ✅, one Important, four minors. Fold into owning commits where clean.

1. **Important:** priming out-of-memory is unreachable. `WasmtimeEngine.cpp` compilePriming/runPriming (~385–430) and `WamrThreadEnv.cpp:77,128` map every failure to `RuntimeUnavailable`. Map genuine allocation failures (null engine/store/module/instance from an allocation, WAMR's out-of-memory error text or code, Wasmtime's resource errors where distinguishable) to `OutOfMemory`; keep the rest `RuntimeUnavailable`. Test that an injected allocation failure in priming yields the transient `OutOfResources` refusal end to end (scripted Hello is not enough — drive the engine path, via a seam if needed).
2. `vtextension.warning` category: define it at namespace scope like `connectionLog` (ProcessHostConnector.cpp:35) so it is registered before the startup filter applies and can be listed/disabled; make the header doc true.
4. WAMR patch idempotent: apply with `patch -p1 -N --forward` semantics that succeed when already applied (or check with `--dry-run -R` first), and make the patch content part of the CPM source-cache key (e.g. patch file hash in the cache name / PATCHES content). Prove: reconfigure on an already-patched `_deps/wamr-src` succeeds.
5. Configure summary for a system-installed WAMR states that the cluster-list patch is not applied (user-facing CMake text must not name the engine — say "extension host runtime (system package, without Contour's thread-safety patch)" or similar).

Ruling on minor 3 (ExecutorThread test): keep the flake fix; rename the test to what it now checks, no new assertion.

Verify: ASan targeted vtextension suites, a reconfigure run for 4, clang-tidy diff, lints, clang-format 22. Report: task-5.8-fixwave-round2-report.md.
