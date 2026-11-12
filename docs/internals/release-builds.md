# Release builds

The packages Contour ships are built with link-time optimization and each platform's exploit mitigations, for
Contour's own code only. One CMake option, `CONTOUR_PRODUCTION_BUILD` (default `OFF`), turns it on.

## Where it is on

| Build | How |
|---|---|
| Windows packages (MSI, ZIP) | the `msvc-release` preset |
| Ubuntu `.deb` packages | the `gcc-release` preset |
| macOS image | the `macos-package` preset |
| `contour-daemon` `.deb` and the AppImage | `-DCONTOUR_PRODUCTION_BUILD=ON` in their build steps |
| Fedora RPMs | not set: `rpmbuild` exports Fedora's hardened flags and LTO before `%build`, and CMake takes them from the environment, so setting the option as well would apply them twice. The CI log of the RPM build shows `-flto=auto`, `-D_FORTIFY_SOURCE=3`, `-fstack-protector-strong`, `-fstack-clash-protection`, `-fcf-protection` and the `redhat-hardened-cc1` spec on its compile lines, and `-Wl,-z,relro`, `-Wl,-z,now`, `-Wl,--as-needed` and the `redhat-hardened-ld` spec on its link lines. |

CI's tests run on these binaries, so the optimized and hardened build is the one that is tested. The developer presets
(`clang-release`, `clang-asan`, ...) keep their fast incremental builds. `gcc-release` carries the option because it
is the preset the `.deb` is built with; developers who want a fast release build use `clang-release`. Anyone can turn
the option on anywhere, for example to measure with the packages' flags:

```sh
cmake --preset clang-release -DCONTOUR_PRODUCTION_BUILD=ON
```

## What it does

Only Contour's own targets get the flags: the module is included after the dependencies (vcpkg ports, Qt, CPM's
`_deps`, `vendor/core-cpp`) are added and before `src/` and `examples/`. The hardening flags are the rows of one table
in `cmake/ProductionBuild.cmake`, each verified with a compiler or linker flag check; a flag the toolchain rejects is a
configuration error, not a silent omission. `/Ob2` comes from the small per-platform table above it, and the
link-time optimization options are CMake's own for the toolchain.

| Platform | Flags | What they buy |
|---|---|---|
| all | whole-program optimization (`/GL` + `/LTCG` on MSVC, `-flto=auto` on GCC, ThinLTO on Clang) | smaller, faster code |
| Windows | `/Ob2`, `/Gw /Gy`, `/OPT:REF /OPT:ICF` | full inlining, dead code and duplicate functions removed |
| Windows | `/guard:cf` (compile and link) | Control Flow Guard |
| Windows | `/CETCOMPAT` | compatible with CET user-mode shadow stacks |
| Linux | `-fPIE`, `-pie` | a position-independent executable (ASLR) |
| Linux | `-fstack-protector-strong`, `-fstack-clash-protection` | stack smashing and stack clash detection |
| Linux | `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3` | fortified C library calls |
| Linux, x86-64 | `-fcf-protection` | CET instrumentation; a third-party object without the marking clears it from the binary |
| Linux | `-Wl,-z,relro -Wl,-z,now -Wl,--as-needed` | full RELRO, immediate binding, no unused libraries |
| macOS | thin LTO (also for the Objective-C and Objective-C++ sources), `-Wl,-dead_strip` | smaller, faster code |

The macOS hardened runtime already exists for notarization and is not part of this.

### Position-independent code

Every first-party translation unit is compiled with `-fPIE` and the executables are linked with `-pie`, as rows of the
flag table. `CMAKE_POSITION_INDEPENDENT_CODE` is deliberately not used: it compiles every static library with `-fPIC`,
whose calls to external functions and data go through the PLT and GOT, and that cost 2 to 3 % of throughput in the `tui`
workload of `bench-headless`. Removing the hardening flags one at a time, or all of them, changed nothing; removing only
PIE, with LTO and every mitigation kept, brought the throughput back, and the explicit `-fPIE` keeps it. No first-party
shared or module library is built, so `-fPIE` objects are enough, and the configure fails if one ever is. Code that
links Qt keeps the `-fPIC` that Qt requires of it; the terminal engine and the libraries below it are `-fPIE`. The
throughput gain was measured on `bench-headless`, which links no Qt, so it holds regardless.

### What is refused

A configuration that would not be the shipped one stops at configure time. The first three are refused at the top of
the configure, before CPM and FetchContent fetch their dependencies (the vcpkg presets install their ports even earlier,
in `project()`):

- a build type other than `Release` (the packages are `Release` builds, and so are the binaries CI tests);
- `CONTOUR_SANITIZE` other than `OFF`;
- coverage (`CONTOUR_CODE_COVERAGE_ENABLED`).

After all targets exist, the configure also fails on a first-party shared or module library, which `-fPIE` objects
cannot be linked into.

A toolchain without a flag table is refused where the flags are applied, after the fetch: anything but MSVC on Windows,
GCC or Clang on Linux, and AppleClang or Clang on macOS.

Multi-config generators and `clang-cl` are not refused by a message of their own. `clang-cl` fails the toolchain test.
A multi-config generator is caught by the build-type test only while `CMAKE_BUILD_TYPE` is unset; with it set to
`Release` the other configurations get the flags too, so use a single-config generator.

## Debug symbols

No package comes with debug symbols, and none is published separately: there is no `-dbgsym` `.ddeb`, no Fedora
`-debuginfo` or `-debugsource` RPM, no Windows PDB zip and no macOS dSYM zip, and the AppImage carries no debug
information either. To debug or profile an optimized build, configure the `clang-profile` preset (`RelWithDebInfo`,
without the production flags).

## What `check_production_build` asserts

`scripts/check-production-build.py` runs as the ctest `check_production_build` in the production builds with tests (the
AppImage, which has none, runs it as a CI step), and its self-test, `check_production_build_selftest`, runs
everywhere. It reads the build's `compile_commands.json` and the linked
binary:

- every first-party translation unit carries each flag of the manifest `production-build.json`; no third-party
  translation unit carries one;
- the binary has the properties the flags are for: Control Flow Guard, CET compatibility and ASLR on Windows; a PIE with
  RELRO, immediate binding and the stack protector on Linux.

macOS has no binary row: its only ones were about the dSYM, which no build produces any more, and no hardening property of
a Mach-O binary is checked instead, so on macOS the compile database is all there is. The check does not probe a Mach-O
binary, and `dwarfdump` is no longer required.

It proves the compile flags and the binary's properties. It does not prove that a link-only flag (`/OPT:REF`, `/OPT:ICF`,
`-Wl,--as-needed`, `-Wl,-dead_strip`) reached the linker, because link options are not in the compile database.

## What it costs

Measured locally on 2026-11-07 on a Ryzen 9 9950X3D, only while the CPU was idle, as the best of three alternating
rounds. Windows is MSVC against `origin/master` 4d567d4e4. Linux is GCC 15 (WSL Ubuntu 26.04) with `gcc-release`: the
baseline is the same preset without the production flags, built from the commit before this change (e000176d4), and
"production" is the shipped flag set, `-fPIE` included.

| | Windows (MSVC) | Linux (GCC 15) |
|---|---|---|
| main binary | 36.3 MB -> 15.8 MB (-57 %) | 30.9 MB -> 26.6 MB stripped (-14 %) |
| link time of the main binary | about 3 to 6 s -> about 43 to 48 s | about 5 to 8 s -> about 45 to 50 s |
| `bench-headless throughput` | +13 % to +44 % per workload | -0.6 % (`tui`, primary) to +43 % per workload |

Links are slower because optimization moves to the link. The `tui` workload was 2 to 3 % slower while the static
libraries were built with `-fPIC` (see above); with `-fPIE` it is within a percent of the baseline.

### CI measurements

Measured on GitHub's runners on 2026-11-07, while the packages were still `RelWithDebInfo` builds that published their
symbols, from workflow run 42372326441 of pull request #2250, with the packaging
steps switched on for the pull request. The base is the `master` run 42334939141 (commit 130538887). Read the job
times with their compiler-cache state in mind: `master`'s jobs ran with warm caches (37 to 80 % ccache hits), while
every job of the pull request ran with 0 to 1 % hits, except macOS in the second attempt (9.5 %). The pull request's
caches had been evicted by the repository's 10 GB cache budget between the attempts, so neither attempt is warm and
the times below are closer to a cold build than to `master`'s.

| Job | `master` | attempt 1 | attempt 2 |
|---|---:|---:|---:|
| Windows | 61 min 37 s (62 % hits) | 93 min 07 s (0.3 %) | 114 min 49 s (0.3 %) |
| macOS (arm) | 96 min 40 s (41 %) | 137 min 02 s (1.2 %) | 127 min 57 s (9.5 %) |
| macOS without the comparison step | 96 min 40 s | 109 min 13 s | 101 min 48 s |
| Packaging for Ubuntu 26.04 | 71 min 21 s (38 %) | 79 min 55 s (0.9 %) | 88 min 30 s (0.9 %) |
| Packaging for AppImage | 15 min 21 s (79 %) | 26 min 59 s (0 %) | 28 min 32 s (0 %) |
| `contour-daemon` package, Ubuntu 24.04 | 10 min 16 s (80 %) | 23 min 09 s (0 %) | 16 min 10 s (0 %) |

A `master` run with an equally cold cache gives the fair comparison: the macOS job on `master` took 109 min 54 s with
0.9 % hits (run 42101659313), and the pull request's macOS build step took 4653 s and 4335 s against 4709 s there. The
nearest `master` Windows and Ubuntu runs with the lowest hit rates took 95 min 20 s (55 %) and 78 min 55 s (32 %),
against 93 and 115 min, and 80 and 89 min. The AppImage and `contour-daemon` jobs have no cold `master` run to compare
with; their build steps (1488 s and 1203 s in the first attempt) are about twice `master`'s warm ones (719 s and
475 s).

Package sizes, in bytes:

| Package | `master` | pull request |
|---|---:|---:|
| `contour.exe` in the ZIP | 36,386,304 | 15,790,080 |
| MSI | 74,206,961 | 69,955,313 |
| Windows ZIP | 74,626,984 | 70,376,812 |
| Ubuntu 26.04 `contour` `.deb` | 12,035,200 | 10,445,624 |
| `usr/bin/contour` in that `.deb` | 31,000,816 | 26,620,608 |
| AppImage | 275,728,888 | 215,968,248 |
| macOS `.dmg` | 92,881,315 | 93,040,172 |

On macOS the same tree built twice in the job, with and without the production flags, with `bench-headless`
(`-DLIBTERMINAL_BUILD_BENCH_HEADLESS=ON` for both): the `contour` binary went from 37,356,296 to 36,267,192 bytes (-2.9 %),
and `bench-headless throughput`, three alternating rounds on the shared runner, was between -2.6 % and +14.2 % per workload over the two attempts (best of the rounds of each). The one value below -2 % (ascii on the primary screen in the second attempt) was +4.6 % in the first, so it is runner noise. In the second attempt, the link of `contour` took 312 s with the production flags against 2 s without (the production figure included extracting the dSYM, which is gone).

The `check_production_build` test passed on every platform.

## Measuring with the packages' flags

Configure any tree with `-DCONTOUR_PRODUCTION_BUILD=ON` and a `Release` build type. Expect a slower link and
faster code; `bench-headless` is the tool for the throughput comparison.
