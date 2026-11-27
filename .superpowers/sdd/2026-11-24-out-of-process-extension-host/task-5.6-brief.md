### Task 5.6: macOS sandbox and the helper's entitlement
**Files:**
- Create: `host/Sandbox_macos.cpp`, `host/macos/SandboxProfile.hpp`, `support/macOS/contour-extension-host.entitlements`.
- Modify: `scripts/macos-bundle.py` (per-helper entitlements), `.github/workflows/build.yml` (the macOS ad-hoc hardened-sign step and the helper suite run), `CMakeLists.txt` (`Darwin` appended; D110).

**Produces:**
- `SandboxProfile` (a constexpr profile text) and its parameter rows `{name, value}`. `SHM_PREFIX = "/contour-eh-"` comes from `SharedObjectName.hpp`.
- Hand-declared `extern "C" int sandbox_init_with_parameters(const char*, uint64_t, const char* const[], char**)` and `void sandbox_free_error(char*)`.
- `lockDown()` applies the profile. An error → `Profile`, with `errorbuf` told.

Profile: `(version 1) (deny default)`, plus:
- `(allow ipc-posix-shm-read-data ipc-posix-shm-write-data (ipc-posix-name-prefix (param "SHM_PREFIX")))`;
- `(allow signal (target self))`;
- `(allow sysctl-read (sysctl-name …))`, the names taken from CI's first `sandbox` deny log;
- no `network*`, no `mach-lookup`, no `file-read*`, no `process-fork` or `process-exec`.

- [ ] **Step 1: Write the tests** (macOS CI only).
  - The probe ops table gains macOS expectations (open → `EPERM`, socket → `EPERM`, fork → `EPERM`, `mach-lookup` of `com.apple.system.logger` → denied).
  - `"Sandbox.profileHasNoNetworkOrMachLookup"` (compiles everywhere; a text table test on the profile string).
- [ ] **Step 2: Implement the profile and the declaration.** Wasmtime's mach ports are already off (`wasmtime_config_macos_use_mach_ports_set(false)`); the fact is restated in the file comment.
- [ ] **Step 3: Implement the entitlement.**
  - `contour-extension-host.entitlements` holds `com.apple.security.cs.allow-unsigned-executable-memory` only.
  - `macos-bundle.py` signs `Contents/Helpers/contour-extension-host` with `--options=runtime --entitlements <helper file>` before the outer bundle. The outer bundle's entitlements are unchanged.
  - A unit test in the script's own test file asserts the helper path gets the helper file and nothing else does (or an equivalent check run under `python3 -I`).
- [ ] **Step 4: Add the CI case.**
  - The macOS leg runs `codesign --force --sign - --options=runtime --entitlements support/macOS/contour-extension-host.entitlements <helper>`, then `vtextension_helper_test "~[.measure]"` with `RequireLockedDown`.
  - A second run signs without the entitlement and expects the loads to fail, which proves the entitlement is load-bearing.
- [ ] **Step 5: Verify by reading** against a checklist:
  - the declaration matches Chromium's;
  - `-Werror` sees no deprecated symbol;
  - shm grants are read/write-data only, never create (R38);
  - `MAP_SHARED|MAP_FIXED` of a received shm descriptor needs no `file-map` rule;
  - Phase 4's open macOS items: `CMSG_SPACE` as an array bound, BSD descriptor attribution.
- [ ] **Step 6: CI** only if the owner allows a push; otherwise mark it outstanding.
- [ ] **Step 7: Commit** (two):
  - `feat(vtextension): the extension host runs in a deny-by-default sandbox on macOS`
  - `build: sign the macOS extension host with the one entitlement its compiler needs`


---

## Plan decision (verbatim)

- D105. macOS:
  - a constexpr deny-default profile with parameters, shm read/write-data by prefix only (R38), with no network, mach-lookup, file reads, fork or exec;
  - a hand-declared `sandbox_init_with_parameters`;
  - `allow-unsigned-executable-memory` on the helper only, through `support/macOS/contour-extension-host.entitlements`, with the helper signed before the bundle;
  - a CI case with and without the entitlement.
  - No parent-death binding remains (documented, spec §5.3).

## Item 0: carried from the review of the FreeBSD sandbox (first, as its own commit)

1. The automatic engine choice (`CMakeLists.txt:473-478`) picks `wasmtime` whenever Wasmtime is found and extensions run out of process. On FreeBSD the sandbox then refuses that engine with FATAL_ERROR, even though the user chose no engine. Make the automatic rule fall back to `wamr` on FreeBSD (data-driven: a per-platform row, not an ad-hoc `if`). An explicit `wasmtime` on FreeBSD stays refused.
2. `AddressSpaceHeadroomBytes` (`HostBootstrap.hpp:91-96`) is justified in prose by `MaxLaneChannels × ExecutorStackBytes`. Add a `static_assert` that ties them, so growing either fails to compile instead of silently shrinking the headroom.

## Controller notes (binding)

- **macOS CI runs on pull requests.** The jobs are `macOSArm` and `macOS (no GUI frontend)` on draft PR #2431, so this sandbox CAN be proven before merge. You cannot push; I push after your report. Plan for iteration:
  - Make the CI steps print the sandbox's deny log on failure, so the first CI run tells us which `sysctl-read` names (and anything else) the profile must allow. For example, run `log show --last 5m --predicate 'sender == "Sandbox"'` (or `--style compact`) after the helper suite when it fails.
  - Start with the minimal allow-list the plan names. Add any `sysctl` names you can justify from WAMR, Wasmtime or libc reading, each with its reason.
- **Do NOT append `Darwin` to `_CONTOUR_SANDBOXED_PLATFORMS` in this task (D110).** macOS joins only at the gate, once the macOS CI legs are green with the sandbox. Land the code so that it is compiled and its tests run on macOS CI (the helper suite under `RequireLockedDown` in the CI step), while `contour` itself stays in-process on macOS for now.
- **The `[sandbox]` tests and the probe ops table** must compile everywhere. `Sandbox.profileHasNoNetworkOrMachLookup` runs on Linux too.
- **Check by reading** against the Step 5 checklist, and cite sources: Chromium's `sandbox/mac/seatbelt.cc` declaration, Apple's sandbox profile language as documented by the community (e.g. reverse-engineered SBPL references), and WebKit's or Chromium's `.sb` files for the `ipc-posix-shm*` operations and `ipc-posix-name-prefix`. Phase 4's open macOS items are `CMSG_SPACE` as an array bound and BSD descriptor attribution. Read the phase-4 facts in `progress.md` for context; skim only.
- **CI traps:**
  - Apple clang turns an unused variable into an error.
  - `-Werror` turns deprecated declarations into errors. That is why `sandbox_init_with_parameters` is hand-declared and not included from `<sandbox.h>`.
  - Read `task-ci1-report.md` for the others.
- **Before you report, run locally:**
  - `ctest -R 'vtextension|wasm_runtime_containment'`;
  - `ctest -L lint` (it includes `check_workflows`, which parses `build.yml`);
  - CI's tidy-diff;
  - clang-format, typos and editorconfig on the changed files.
  - The Python script change runs under `python3 -I`, with its test.
- **Commits:**
  - Item 0 first, then the two commits the brief names.
  - Each commit builds on its own.
  - `git commit -s`, with no trailers.
  - Messages never mention the plan, task numbers or "phase".
  - User-facing text never says wasm, WAMR or Wasmtime.
