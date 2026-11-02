# `contour-daemon` packages — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a Qt-free `contour-daemon` package (`.deb`, `.rpm`) and make `contour daemon-service` behave correctly on a machine with only the packaged unit.

**Architecture:** The package is the existing `CONTOUR_FRONTEND_GUI=OFF` binary with the GUI-only install rules gated off; it conflicts with the GUI `contour` package. A CI job replaces `build_headless` and also builds, installs and tests the package.

**Tech Stack:** C++23, CMake/CPack (DEB), RPM spec, GitHub Actions, Catch2.

**Spec:** `docs/superpowers/specs/2026-11-02-contour-daemon-packages-design.md`

## Global Constraints

- Package name `contour-daemon`; `contour` and `contour-daemon` `Conflicts:`/`Replaces:` each other; no `Recommends`/`Suggests` either way.
- The daemon RPM also `Conflicts:` with Fedora's `contour-terminal`.
- No listening port, no system unit, no dedicated account, no maintainer script that creates users or touches per-user state. The packaged user unit is installed but not enabled.
- Lingering is never enabled; only detected and explained.
- Project rules (`AGENT.md`): `.hpp`, `#pragma once`, `enum class : uint8_t` over `bool` (zero = negative case), `std::expected` for fallible APIs, Doxygen on new public API, range-for only, `-Werror` with no `NOLINT`, DI for I/O, new code lands with Catch2 tests. Never edit `vendor/core-cpp`.
- Commits: `Signed-off-by: Christian Parpart <christian@parpart.family>` only; no AI attribution lines.
- Ubuntu 24.04 is the daemon `.deb` target (26.04 also if cheap); bundled extensions ship in the `.deb`, not the RPM.

## Review Focus

1. A machine with the packaged vendor unit but no user registration reads as *not installed*; after `install` it reads installed; after `uninstall` it reads not installed again (Task 1).
2. `loginctl` missing, failing, reporting garbage, or no user name available: lingering reads Unknown, install still succeeds, no hint is printed for Unknown (Task 2).
3. Installing `contour` over `contour-daemon` and vice versa succeeds and leaves exactly one `/usr/bin/contour` (Task 4 container test, Task 5 for RPM).
4. The GUI `.deb`'s file list and Depends stay unchanged apart from the new `Conflicts`/`Replaces` (Task 3).

---

### Task 1: User-registration semantics for the systemd backend

**Files:**
- Modify: `src/vthost/ServiceControl_systemd.cpp` (`SystemdUserBackend::status`, `uninstall`)
- Test: `src/vthost/ServiceControl_systemd_test.cpp`

**Interfaces:**
- Produces: `SystemdUserBackend::status()` **and `uninstall()`** treat a service as installed only if the user's own unit file (`_config.unitPath`) exists, regardless of a vendor unit systemd has loaded (`uninstall()` currently disables the loaded vendor unit and reports success). No change to `ServiceBackend`, `ServiceStatus` or `ServiceRunState`.
- Side effect to state and test: `findRegisteredService` (`src/vthost/DaemonStarter.cpp`) keys off `status()`, so on a packaged machine the client's auto-start no longer `start`s the vendor unit and falls back to spawning the daemon itself.

- [ ] **Step 1: Read `status()` and its existing tests; confirm** that a loaded vendor unit with no user file yields `Stopped` today. Note the result in the commit message.
- [ ] **Step 2: Write failing tests** in `ServiceControl_systemd_test.cpp`: `"systemd status: vendor unit loaded, no user unit file -> NotInstalled"` (fake `CommandRunner` answers `LoadState=loaded`, `ActiveState=inactive`; fake filesystem has no `unitPath`; `REQUIRE(status->state == ServiceRunState::NotInstalled)`), and `"systemd status: user unit file present -> Stopped/Running per ActiveState"`. Also: `uninstall()` on a vendor-only machine runs no `disable` and reports `NotInstalled`; after install then uninstall, `status()` is `NotInstalled`; `ServiceDaemonStarter` on a vendor-only machine spawns instead of starting the vendor unit.
- [ ] **Step 3: Run** `out/clang-asan/src/vthost/vthost_test "systemd*"`. Expected: the new cases FAIL.
- [ ] **Step 4: Implement** the file-exists check in both functions (use the injected `_fileSystem`), keeping `ExecStart` parsing for the present case.
- [ ] **Step 5: Run** the vthost suite; expected PASS.
- [ ] **Step 6: Commit** the spec, this plan and the change: `vthost: report the systemd daemon as installed only when the user registered it`.

### Task 2: Lingering detection and the CLI hint

**Files:**
- Create: `src/vthost/Linger.hpp`, `src/vthost/Linger.cpp`; Modify: `src/vthost/CMakeLists.txt`, `src/contour/cli/ContourApp.cpp`
- Test: `src/vthost/Linger_test.cpp`

**Interfaces:**
- Produces, namespace `vthost`:
  ```cpp
  enum class LingerState : uint8_t { Unknown = 0, Off, On };
  [[nodiscard]] LingerState lingerStateOf(CommandRunner& commands, std::string_view user);
  [[nodiscard]] std::string_view lingerAdvice(); // one sentence + `loginctl enable-linger` command
  ```
  Runs `loginctl show-user <user> -p Linger`; `Linger=yes` → On, `Linger=no` → Off, anything else (missing program, non-zero exit, other text) → Unknown. The function is pure over `CommandRunner` output with **no platform check**, so its tests run on the macOS and Windows jobs too; the platform and systemd-path gate lives at the call site. `user` is the invoking user's login name from `core::Environment` (`USER`); an empty `user` returns Unknown without running anything (with no user, `loginctl show-user` describes the manager).

- [ ] **Step 1: Failing tests** using `src/vthost/testing/FakeCommandRunner.hpp`: `yes`, `no`, garbage output, program-not-found, non-zero exit → On, Off, Unknown, Unknown, Unknown; plus empty user → Unknown with no command run.
- [ ] **Step 2–4: Run (FAIL), implement, run (PASS).**
- [ ] **Step 5: Wire the CLI hint:** in `daemon-service install` (`ContourApp.cpp`, systemd Logon path only), after a successful install print `lingerAdvice()` when state is `Off`; print nothing for On/Unknown. Add a test in the existing `daemon-service` test style.
- [ ] **Step 6: Commit** `vthost: tell a systemd user when their daemon will not outlive their login`.

### Task 3: The `contour-daemon` Debian package

**Files:**
- Modify: `src/contour/CMakeLists.txt` (Unix install branch ~L1962–2062 and the CPack variables)
- Test: per-configuration ctest checks in the style of `contour_e2e_cli_verbs` (`src/contour/CMakeLists.txt` ~L1589): the GUI configuration asserts desktop/icons/metainfo are in the install rules and `Conflicts: contour-daemon` is set; the GUI-less configuration asserts they are absent and the package is named `contour-daemon`. The real install-and-list check of the GUI-less half runs in Task 4's job (a GUI build tree cannot produce it).

**Interfaces:**
- Produces: with `CONTOUR_FRONTEND_GUI=OFF` on Linux, `cpack` yields `contour-daemon_<ver>_<arch>.deb` containing the binary, shell-integration, terminfo, bundled extensions, fonts, licence, and the user unit — and none of: `.desktop` files, KIO menus, hicolor icons, metainfo, the OOM drop-ins for `app-*` units. `Depends` is `libfreetype6, libharfbuzz0b, libyaml-cpp<N>, libssh2-1`, plus `libutempter0` and `libsystemd0` (when `CONTOUR_WITH_SYSTEMD`); derive the final list with `ldd`/`dpkg-shlibdeps` on the real binary and keep the yaml-cpp release table. `Section: admin`; no `Recommends: xdg-open` in the daemon branch. The CPack file name is `Contour-<ver>-Linux-contour.deb` from `CPACK_PACKAGE_NAME "Contour"` plus the component, renamed by CI today; set `CPACK_DEBIAN_CONTOUR_PACKAGE_NAME "contour-daemon"` and `CPACK_DEBIAN_CONTOUR_FILE_NAME` (or a matching `mv` in Task 4). Package-level `Conflicts: contour`, `Replaces: contour`; the GUI package gets `Conflicts: contour-daemon`, `Replaces: contour-daemon`.

- [ ] **Step 1: Failing checks:** the per-configuration ctest checks above, plus a configure-time assertion that `CPACK_DEBIAN_PACKAGE_DEPENDS` contains no `qt`/`qml` when the GUI is off. Run them; FAIL.
- [ ] **Step 2: Gate** the GUI-only `install()` lines and the Qt `Depends` on `CONTOUR_FRONTEND_GUI`; set the daemon `CPACK_DEBIAN_*` variables in the else-branch. Handle the component-name override so the file is named `contour-daemon`. Keep the dbgsym package.
- [ ] **Step 3: Run** the checks; PASS. Also run the full GUI configure to confirm the GUI `.deb` file list and Depends are unchanged apart from the new `Conflicts`/`Replaces`.
- [ ] **Step 4: Commit** `packaging: build a Qt-free contour-daemon Debian package`.

### Task 4: CI job replacing `build_headless`, and release assets

**Files:**
- Modify: `.github/workflows/build.yml` (remove `build_headless`, add `package_daemon_for_Ubuntu`; `do_release` `needs:` and asset list), `docs/internals/build-times.md` (~L141, L148, names `build_headless`), possibly `scripts/check-workflows.py`

**Interfaces:**
- **No `if:` condition on the job's build, test, `ldd` and package-install steps**: like `build_headless` it runs on every PR (its comment says an unbuilt GUI-less configuration is how it rotted before); only the upload/release attachment may be conditional on `release`/non-PR. Package with `cpack --config <build>/CPackConfig.cmake` (the `gcc-release` preset's build directory differs).
- Produces: a job on `ubuntu-24.04` (matrix adds `26.04` only if cheap) that carries over every guard of the removed job: configure with `CONTOUR_FRONTEND_GUI=OFF`, `CMAKE_DISABLE_FIND_PACKAGE_Qt6=ON`, build `contour` and `contour_test`, run `contour_test|contour_e2e_cli_verbs|contour_e2e_shell_integration`, `ldd` shows no `libQt`. Build type is RelWithDebInfo with `CONTOUR_TESTING=ON` (state the choice in the PR). Then `cpack`, and in a clean `ubuntu:24.04` container: `dpkg -i` (no Qt pulled in; `dpkg -s` Depends has no `qt`/`qml`), `contour version` and `contour help | grep "contour daemon"`, user unit and the `agent-status clock git plasma sysload` extensions present; then `dpkg -i` the GUI `.deb` over it and assert one `/usr/bin/contour` and `dpkg -s contour-daemon` is gone, and the reverse.

- [ ] **Step 1: Add the job** copying `build_headless`'s apt/Rust/clang steps; add the package and container steps. **Step 2: Remove `build_headless`**; add the new job to `do_release.needs` and attach `contour-daemon-<ver>-ubuntu<rel>-amd64.deb` (plus dbgsym) the way the GUI `.deb` is attached. **Step 3: Run** `python3 scripts/check-workflows.py`; PASS. **Step 4: Push the branch and read the workflow run**; the job must go green.
- [ ] **Step 5: Tell the maintainer** in the PR description that if `build_headless` is a required status check, it must be replaced by the new job's name (not verifiable from the repo).
- [ ] **Step 6: Commit** `ci: package and install-test contour-daemon, replacing build_headless`.

### Task 5: The RPM

**Files:**
- Modify: `.github/fedora/contour.spec`, `.github/fedora/Dockerfile`; `.github/workflows/build.yml` (revive an RPM job for current Fedora and rawhide; add an EPEL job only if feasible; extend `do_release` to attach the `.rpm`s)

**Interfaces:**
- Produces: `rpmbuild --with daemon` builds `contour-daemon` from the same spec: a `%bcond_with daemon` selects `-DCONTOUR_FRONTEND_GUI=OFF`, `Name: contour-daemon` (with `Source0`, `%setup -n` and `URL` pinned to the literal `contour`, since they derive from `%{name}` today), a Qt-free `BuildRequires`/`Requires` list (`freetype`, `harfbuzz`, `libssh2`, `yaml-cpp`, `libutempter`, `systemd-libs`), `Conflicts: contour contour-terminal`; the default build keeps `Name: contour` and gains `Conflicts: contour-daemon` and loses the wrong `Requires: qt6-qtmultimedia-devel`. `%files` for the daemon omits desktop/icon/metainfo and the `app-*` drop-ins glob, and lists `%{_prefix}/lib/systemd/user/contour-daemon.service`; the GUI `%files` gains that unit too (CMake already installs it, so today's spec fails "installed but unpackaged"). Extensions stay off (`-DCONTOUR_EXTENSIONS_BUNDLED=OFF`).

- [ ] **Step 1:** Update the Dockerfile base from `fedora:35` to a current Fedora and make the container run both builds (`rpmbuild` in its build-all mode and `rpmbuild` in that mode with `--with daemon`). **Step 2:** edit the spec. **Step 3:** Build both in the container; `rpm -qpR` the daemon RPM must list no `qt6` requirement; `rpm -i` it in a clean Fedora container and run `contour version` and `contour help | grep "contour daemon"`; installing the GUI RPM over it must fail with the declared conflict. **Step 4:** Decide EL9 (gcc-toolset) vs EL10-only in the PR; if EL9 is impractical, say so. **Step 5: Commit** `packaging: build a Qt-free contour-daemon RPM`.

### Task 6: Docs and release note

**Files:**
- Modify: `docs/install.md`, `docs/persistent-sessions.md` (or the page `Documentation=` in the unit points to), `metainfo.xml` (release entry)

- [ ] **Step 1:** Document: server install (`apt install ./contour-daemon_*.deb`, `dnf install contour-daemon`), that installing `contour` already includes the daemon, `contour daemon-service install`, the lingering note, and reach via SSH only. **Step 2:** Add the release note via the repo's `add-release-note` convention. **Step 3:** `scripts/check-spelling.py` and the docs build pass. **Step 4: Commit** `docs: install and enable contour-daemon`.

### Task 7: Whole-branch gate

- [ ] Run `/simplify`, then `ctest --preset=clang-asan`; zero warnings.
- [ ] Run the final `/code-review` (project rule) and address every finding.
- [ ] Report in the summary: performance impact (none expected), risk, coverage of the new files.
