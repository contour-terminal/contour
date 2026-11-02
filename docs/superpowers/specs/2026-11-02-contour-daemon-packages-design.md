# `contour-daemon` packages

Date: 2026-11-02. Status: draft for review.

## Intent

Daemon mode is a first-class way to run Contour: `contour daemon` hosts persistent terminal
sessions that clients attach to. The natural home for a daemon is a server, where a GUI and its
dependencies (Qt, QML modules, Wayland/X11 plugins) should not be installed at all. Today the only
Linux package is the GUI `.deb`, whose `Depends` pulls in about twenty Qt packages.

This work provides a Qt-free binary package, **`contour-daemon`**, for Ubuntu (`.deb`) and for the
Fedora/RHEL family (`.rpm`).

(A settings-page action to register the daemon service was designed alongside this and
deliberately dropped from this work; it may return as its own change.)

## Goals

- `apt install ./contour-daemon_*.deb` / `dnf install contour-daemon` on a minimal server pulls in
  no Qt and no X11/Wayland libraries.
- The user then runs `contour daemon-service install` and has a daemon that starts with their
  login session (and, with lingering, survives logout).
- `daemon-service status` and `uninstall` behave correctly on a machine where only the packaged
  unit exists.

## Non-goals

- Replacing or duplicating an SSH server. The package opens **no listening port** and ships no
  firewall, TLS or authentication setup. Remote reach is through SSH, using the existing
  `contour client` / attach paths.
- A system-wide service or a dedicated service account. The daemon is a **per-login-user
  service**.
- A GUI action for registering the service (dropped, see Intent).
- Hosting a PPA or COPR repository (a possible later step; releases attach the files first).
- Shipping the bundled WASM status-line extensions on RPM until the offline Rust build is solved.

## Anti-goals (would be a failure even if it technically works)

- `contour` and `contour-daemon` installable together with both owning `/usr/bin/contour`.
- A `Recommends`/`Suggests` from either package to the other: apt installs recommends by default,
  which would pull in a conflicting package.
- The package silently changing login/session policy (lingering).
- A package maintainer script creating users or touching per-user state.

## Decisions

### Packages

- **Name: `contour-daemon`** (named for what the user gets, not for how it is built).
- It is the **same program** as `contour`, built with `CONTOUR_FRONTEND_GUI=OFF`. The GUI-less
  binary already exists and is gated in CI (`build_headless`, which asserts it links no Qt).
- Both packages install `/usr/bin/contour`, so they are **mutually exclusive**:
  `contour` has `Conflicts: contour-daemon` and `Replaces: contour-daemon`; `contour-daemon` has
  `Conflicts: contour` and `Replaces: contour`. No `Recommends`/`Depends` either way. The GUI
  package already contains the daemon, so a user of `contour` needs nothing more.
- A third arch-independent `contour-common` package is **not** part of this work.

### Service model

- The service is the **existing systemd user unit** (`contour-daemon.service`), installed to
  `/usr/lib/systemd/user/`, **not enabled by the package**. There is no system unit.
- Enabling is per user: `contour daemon-service install` (already implemented through
  `vthost::ServiceControl`).
- **Lingering** (`loginctl enable-linger`) lets the daemon outlive the user's last login session.
  It is a login-policy change, so Contour never makes it. `daemon-service install` **detects**
  whether it is off on systemd Linux and prints one line explaining it and giving the command.

### Build and install gating

- The Unix install branch in `src/contour/CMakeLists.txt` (desktop entries, KIO service menus,
  hicolor icons, AppStream metainfo, the Qt `Depends`) is gated on `CONTOUR_FRONTEND_GUI`. A
  GUI-less configuration produces a CPack package named `contour-daemon` with a small `Depends`
  (freetype, harfbuzz, yaml-cpp, libssh2, libutempter0, libsystemd0 when `CONTOUR_WITH_SYSTEMD`,
  plus what `ldd` of the real binary shows). The user unit is installed for both packages.
- The Debian `Section` for the daemon package is not `x11`.

### Fedora / CentOS family

- `.github/fedora/contour.spec` selects the daemon build with a `%bcond`, producing the package
  named `contour-daemon` (both packages own `/usr/bin/contour`, so they come from two builds of
  one spec, not two installs into one buildroot). Fedora's own package is `contour-terminal`; the
  daemon RPM `Conflicts:` with both `contour` and `contour-terminal`. The existing `Requires` list
  is corrected (it wrongly requires `qt6-qtmultimedia-devel`).
- Targets: current Fedora and rawhide, and EPEL 9/10 for CentOS Stream, AlmaLinux and Rocky.
  EL9's default GCC is too old for C++23; the builder chooses `gcc-toolset` or EL10-only and says
  which in the PR.
- The RPM CI job is currently commented out in `build.yml` and the Dockerfile targets
  `fedora:35`; reviving it with current Fedora is in scope.

### CI

- **`build_headless` is dropped** and replaced by a daemon-package job that takes over everything
  it guards: configure with `CONTOUR_FRONTEND_GUI=OFF` and `CMAKE_DISABLE_FIND_PACKAGE_Qt6=ON`
  (the build must want no Qt even with Qt on the runner, issue #2042), build `contour` and
  `contour_test`, run `contour_test`, `contour_e2e_cli_verbs` and `contour_e2e_shell_integration`,
  and check the linked binary with `ldd` for `libQt`. It then runs CPack, installs the package
  into a clean `ubuntu:24.04` container, and asserts: no `libQt*` in the package's `Depends`,
  `contour daemon --help` runs, the user unit and the bundled extensions are present. The job runs
  on every PR; only release attachment is conditional.
- The job builds a type that is both testable and shippable (the builder picks; RelWithDebInfo
  with `CONTOUR_TESTING=ON` is the obvious candidate) on **Ubuntu 24.04**, and 26.04 too if cheap.
- If branch protection lists `build_headless` as a required check (not verified here), the
  maintainer must update it; the plan lists this as a manual step.
- Release workflow attaches the `.deb` and `.rpm` to GitHub releases next to the GUI ones.

## Calls made while writing this (author's decisions, not agreed with the maintainer)

1. **"Installed" for the systemd backend means the user's own unit file.** Both packages ship
   `/usr/lib/systemd/user/contour-daemon.service`, so `SystemdUserBackend::status()` (which
   answers `NotInstalled` only for `LoadState=not-found`) reports "installed, stopped" on every
   packaged machine, and `uninstall()` would disable the vendor unit. Both now require the user's
   unit file. No change to the `ServiceBackend` interface. Side effect: the client's auto-start no
   longer starts the vendor unit on a packaged machine and falls back to spawning the daemon.
2. **RPM shape:** as in Decisions above (`%bcond`, `Conflicts:` with `contour-terminal`).
3. **Debian build:** a dedicated release-build job on Ubuntu 24.04 (above).

## Constraints

- Project rules apply: C++23, `.hpp`, `enum class` over `bool`, `std::expected` for fallible
  APIs, zero-warning `-Werror`, dependency injection, tests land with the code, and a release
  note in `metainfo.xml`.
- `vendor/core-cpp` is not edited.
- The bundled extensions are Rust compiled to `wasm32-wasip1`; their crates come from crates.io,
  so offline distro builds cannot build them yet. The daemon RPM ships without them at first. The
  CI-built `.deb` keeps shipping them.

## Left to the builder

- Exact `Depends`/`Requires` lists (derive from the real binary on each target).
- CPack mechanics for producing two differently named packages from two configurations.
- Exact wording of the lingering note.
- Whether the EL9 build is feasible in this PR.
