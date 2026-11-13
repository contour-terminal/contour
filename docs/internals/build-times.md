# Build times

How long Contour takes to compile, measured the same way every time so a change can prove its gain.

## Method

```sh
cmake --preset clang-profile              # RelWithDebInfo, the flags the baselines below were measured with
cmake --build out/clang-profile          # compile_commands.json and generated sources must be current
python3 scripts/measure-compile-times.py --build-dir out/clang-profile --jobs 16 --out build-times-out
python3 scripts/measure-compile-times.py --aggregate-only build-times-out/traces --out build-times-out
```

`clang-profile` has the flags `clang-release` had until CI and the packages moved to `Release`, so the baselines below stay
comparable with new measurements.

The script runs every `.cpp` command of `compile_commands.json` directly (any `ccache`/`sccache`/`pace-cc`
launcher is dropped) with `-ftime-trace`, and writes `report.md`: total CPU seconds split into frontend and
backend, a per-module table, the slowest TUs, the headers with the highest summed inclusive parse time
(`#TUs` and ms/TU alongside), and the most expensive template instantiations. `--filter REGEX` limits the
run to matching sources; `--self-test` (ctest `measure_compile_times_selftest`, label `lint`) checks the
aggregator on embedded traces.

Caveats:

- Per-TU wall time is measured with `--jobs` compiles in parallel, so it includes CPU contention.
  Compare runs only at the same `--jobs` on the same machine.
- Header times are inclusive and overlap (a header's time contains the headers it includes); do not sum
  them across headers.
- The release preset compiles with a launcher-free command, so no cache hides work.

## Baseline

`out/clang-profile` (Clang 22, RelWithDebInfo), master at 4de2e780a, 16 jobs, 32-core machine:
1212 TUs, 4511 CPU-s in total (5m19s wall), of which frontend 3136 s and backend 1300 s.

| Module | TUs | CPU-s | Frontend s | Backend s |
|---|---:|---:|---:|---:|
| src/contour | 396 | 2374 | 1682 | 651 |
| src/vtbackend | 200 | 660 | 425 | 226 |
| src/vthost | 169 | 599 | 394 | 197 |
| generated (moc/autogen, in build tree) | 130 | 237 | 206 | 26 |
| src/vtextension | 53 | 148 | 98 | 48 |
| vendor/core-cpp | 74 | 89 | 64 | 23 |
| src/vtrasterizer | 29 | 89 | 61 | 26 |
| src/vtpty | 47 | 80 | 52 | 26 |
| src/vtcontrol | 22 | 61 | 37 | 23 |
| src/vtconformance | 22 | 61 | 47 | 13 |
| src/text_shaper | 14 | 29 | 19 | 10 |
| src/testing | 16 | 26 | 15 | 11 |
| src/crispy | 17 | 25 | 16 | 8 |
| src/vtworkspace | 10 | 18 | 11 | 7 |
| examples | 10 | 9 | 5 | 3 |
| src/vtparser | 3 | 7 | 4 | 3 |

Top 10 headers by summed inclusive parse time:

| Header | Total s | #TUs | ms/TU |
|---|---:|---:|---:|
| src/contour/config/Config.hpp | 298 | 142 | 2098 |
| src/contour/ContourGuiApp.hpp | 269 | 41 | 6551 |
| src/vtbackend/screen/Terminal.hpp | 260 | 214 | 1217 |
| libstdc++ `<format>` | 245 | 1137 | 216 |
| src/contour/display/TerminalDisplay.hpp | 180 | 60 | 3006 |
| src/contour/session/TerminalSessionManager.hpp | 166 | 49 | 3395 |
| src/contour/config/Actions.hpp | 125 | 156 | 804 |
| libstdc++ `<chrono>` | 122 | 956 | 128 |
| Qt `qqmlprivate.h` | 113 | 182 | 620 |
| Qt `qjsprimitivevalue.h` | 106 | 182 | 581 |

## Where a CI build's time goes

One Linux Clang 22 job, read from its log: `ubuntu-24.04 (Clang 22, C++23, Qt6)` of the pull-request run
41084258466 (job 124161606673, 2026-10-30), a 4-vCPU, 15 GiB runner with 3 GiB of swap.

| Phase | Wall time |
|---|---:|
| `cmake --build` (`-j4`, 1498 ninja steps) | 69 m 44 s |
| `--target test` | 18 m 14 s (the four `contour_gui_test*` runs alone: 16 m 05 s) |
| Whole job | 91 m 50 s |

The 1498 steps are 1211 C++ compiles, 44 C compiles, 9 WebAssembly compiles, 62 links, and a few `rcc`/MOC steps.

- **Compiling dominates.** `ccache -s` at the end of the job: 1285 cacheable calls, 517 hits (40.2 %),
  768 misses; the cache held 0.3 GB of a 0.3 GB limit (124 %) and had run 643 cleanups. Misses are
  full compiles, and the local baseline above puts the whole tree at 4511 CPU-s -- 19 minutes on four
  ideal cores. The runner is not ideal: its four vCPUs are two cores with SMT at a lower clock, and
  70 min x 4 vCPU = 280 vCPU-min is 3.7x the baseline's 75 CPU-min, which is the whole difference
  between "about 20 minutes of compute" and the 70 observed. A cache that hit 90 % instead of 40 %
  would leave about a sixth of the compiles, about 12 minutes if a miss costs the average.
- **Linking does not.** ninja prints only completions, so a link has no duration of its own, but the
  bound is tight where it matters: the last step, the link of `contour_gui_test`, completed 7 s after the
  last compile before it. Static libraries and test binaries link inside the seconds between
  neighbouring steps; `CONTOUR_LINK_JOBS=1` serialises them without ever being the critical path.
- **Memory does not.** The sampler logged 419 points, one per 10 s: peak 9502 MB used of 15 GiB, never
  less than 6485 MB available, so the swap was not a factor.

So the gap is cache misses on slow vCPUs, and the cache is the lever. Why it missed:

- The cache directory was capped at 256 MB while a tree needs about twice that (below); at 124 % full,
  ccache evicts entries the same build is about to need.
- Every run of every job saved a new, timestamped entry, pull requests included. A pull request can
  restore caches made on `master` but `master` cannot restore a pull request's, so those saves served
  one PR only -- and filled the repository's 10 GB quota (11.3 GB in use across 30 entries on
  2026-10-30, 18 of them from one PR), evicting the `master` entries that every PR would have hit.
- Windows had no persistent compiler cache at all: its sccache steps were gated on
  `SCCACHE_GHA_ENABLED`, which the workflow set to `"false"`, so they never ran. The 102-minute
  build compiled every file on every run.

## The compiler cache

Rules, in `.github/workflows/build.yml`:

- **`master` pushes save, everything else only restores** (`save: ${{ github.ref == 'refs/heads/master' }}`,
  and for the Windows job's Pace store an `actions/cache/save` step with the same condition).
  Both restore by key prefix, so a pull request takes the newest entry `master` saved under the same
  key. Merge-queue and release-tag runs restore too and save nothing: a cache saved under a tag's
  scope would serve only that tag and push master's out of the quota.
- **A job saves only what pull requests can use.** The cache-saving jobs build the same
  configuration on every event, so the entry a `master` run saves is the one the next pull request
  restores; no job switches its configuration by event. The production builds (`windows_package`,
  `macos_package`) have no cache at all: their objects carry link-time-optimization code that no
  pull request compiles, and an entry of them was 3 GB. They configure with `-DUSE_COMPILER_CACHE=OFF`,
  so the ccache of the runner image is not picked up as a cache that is empty on every run.
- **`evict-old-files: job`** drops every cached object the saving ccache job did not touch, so an
  entry is one working set -- the current tree -- and does not accumulate dead objects up to
  `max-size`. The Pace store has no such pass: `pace-kv` evicts only when the store reaches
  `--storage-max-disk`, so the cap is what bounds it, set at about three times one plain build.
- **Keys are stable**: one per job. The ccache action appends a timestamp to it; the Pace store's key
  is spelled out in the workflow as `pace-store-windows-msvc-ci-<PACE_VERSION>-<run id>-<run attempt>`,
  and restored by the prefix up to the version.
- **The Pace store is saved only if `pace-kv` was still running** when the job stopped it. A daemon
  that refused its store (a damaged file) has exited by then, and saving the same file again would
  hand it to every later run.
- **Hit-defeating inputs are neutralised workflow-wide**: `CCACHE_BASEDIR` (the checkout),
  `CCACHE_NOHASHDIR`, and `CCACHE_SLOPPINESS=include_file_mtime,include_file_ctime`. There is no
  `pch_defines` because there is no PCH.
- **The tidy-diff job has no cache.** It builds generated headers only and compiles nothing.
- **One generation per key.** The ccache action and the Pace store step save a new entry per run and
  delete none, so on `master` the job `prune_compiler_caches` waits for every job that saves one and
  then `scripts/prune-cache-generations.py` deletes each key's entries but the newest. The two kinds
  of entry are the rows of one table, `scripts/lib/compiler_caches.py`, which both that script and
  `scripts/check-workflows.py` read; nothing else in the cache store is touched. The latter fails when
  a job that saves a compiler cache is missing from the prune job's `needs`.
- **A `master` run has a concurrency group per commit**, so it starts at once and is never
  cancelled. With one group for the branch each run queued behind the last and a third push
  cancelled the one still pending, which left a merge's caches hours late or missing.
- **The full-tree clang-tidy runs once a night** (`clang-tidy.yml`, also on release tags and on
  demand), not on every push: it takes three to five hours of a runner, and the free runners it
  held are the ones pull requests wait for. Pull requests lint the lines they change. What this
  gives up: a finding on a line the change did not touch is reported the next morning, not at
  the merge.
- **A job no pull request runs queues behind its own previous run** (a job-level `concurrency`
  group per ref, job and matrix leg; `scripts/check-workflows.py` requires it). With a group per
  commit alone, consecutive pushes ran all of these side by side: on 2026-11-12 the jobs of a
  second `master` build and of pull requests waited 40 to 90 minutes for a runner.
- **A key no pull request restores has no cache** while the quota is tight. The Linux matrix's GCC
  legs and its arm Clang leg, the 26.04 leg of the daemon package, the Ubuntu packaging job, the
  Windows and macOS package jobs run on `master` alone, and the full-tree clang-tidy nightly.

### Why the cache missed in November 2026

On 2026-11-11 a pull request took about two hours again. `ccache -s` from pull-request runs
43010602755 and 42991625301 and `master` runs 42989124457 and 43087533177:

| Job | Hit rate, pull requests | Hit rate, `master` | Cache size / `max-size` | Cleanups per run |
|---|---:|---:|---:|---:|
| Windows | 1.3-1.7 % | 2.1-2.6 % | 2.0-2.1 GB / 1.5 GB (136-138 %) | 1075-1164 |
| macOS | 3.0-3.8 % | 5.2-5.6 % | 1.1 GB / 0.8 GB (135-140 %) | 971-1035 |
| Linux Clang 22 | 52 % | 82 % | 0.7 GB / 0.7 GB (101 %) | 62-192 |
| `contour-daemon` package, 24.04 | 68 % | 91 % | 0.4 GB / 0.5 GB (81 %) | 0-35 |

- **Windows and macOS recompiled nearly everything on every run, `master` included**, although
  the entry was restored and minutes old. Only these two jobs sat far over `max-size`. The daemon
  package is a production build as well, sat under its cap and hit 91 %, so link-time optimization
  as such does not defeat the cache. Over-cap eviction is the leading explanation and not a proven
  one: 2 % is lower than eviction alone obviously accounts for. The caps were raised above the
  working sets; the hit rates of the runs after that decide it.
- **`master` did not seed reliably.** Its runs were serialised behind a clang-tidy job of 178 to
  293 minutes; of ten completed runs five were cancelled and five failed. Branch protection is
  strict, so a pull request builds on a `master` newer than the commit its cache came from, which
  is the gap between 82 % and 52 % on Linux.
- **Generations piled up.** Two entries each of Windows and macOS were 6.2 GB of the 10 GB quota;
  pull requests kept the older one recently used, so GitHub evicted other keys instead
  (`ccache-appimage` in three runs of three, Linux Clang and the daemon package in one).
- Link-time optimization itself (the production build on Windows, macOS, AppImage and the daemon
  package) cost roughly 20 of 103 build minutes on Windows and 10 of 97 on macOS.

### Why pull requests build the plain configuration

Until 2026-11-12 the `Windows` and `macOSArm` jobs built the production configuration
(`CONTOUR_PRODUCTION_BUILD=ON`: link-time optimization and hardening, presets `msvc-release` and
`macos-package`) on every event, and a pull request's Windows or macOS job took one to two hours.
Measured that day on Windows with a cache that worked (99.0 % hits, 1486 of 1501 compiles): the build
step took 53 minutes, of which compiling was 4.0 and linking 48.8. Every test executable is optimized
again at link time: `contour_test.exe` 1110 s, `contour_gui_test.exe` 382 s, `vtbackend_test.exe`
376 s, `bench-latency.exe` 317 s. No compiler cache can help with that, and the repository was at
GitHub's limit of 20 concurrent jobs with five of them typically held by two-hour Windows jobs.

So a pull request builds the plain configuration, `msvc-ci` and `macos-ci`: the same presets without
`production` (and on macOS without signing and notarization), in `Release`. The jobs `Windows` and
`macOSArm` build it on every event and stay the required checks. The production build, with the full
test suite run against the binaries that ship, `check_production_build`, signing and packaging, is
`windows_package` and `macos_package`, which run on `master` pushes, in the merge queue and on release
tags. The accepted cost: a defect that only link-time optimization or the hardening flags expose is
found on `master` after the merge, not in the pull request. The release still waits for the package
jobs, and for the plain ones too.

`scripts/check-workflows.py` keeps it that way: a job that builds the production configuration --
a `--preset` or value naming a preset that inherits `production`, `CONTOUR_PRODUCTION_BUILD=ON`, or
`scripts/build-appimage.sh`, which sets it -- must not run on pull requests. Its table
`PRODUCTION_ON_PULL_REQUESTS` names the two exceptions, each with its reason: the `contour-daemon`
package, the only GUI-less build, and the AppImage; both are short Linux jobs.

### Sizes

Measured with ccache 4.9.1 (the version the Ubuntu 24.04 runner installs), compression on, a clean
`RelWithDebInfo` build of the whole tree into an empty cache (before CI and the packages moved to `Release`; the rows
are re-measured from the first `master` run after that):

| Tree | Compiles cached | Cache size |
|---|---:|---:|
| Clang 22, `clang-profile` | 1289 | 0.50 GB (497 MB on disk) |
| GCC 16.2, `gcc-release` | 780 of ~1290 before a `-Werror=bidi-chars` stop in `ExtensionConsent_test.cpp` | 0.53 GB, about 0.9 GB extrapolated to the full tree |

The brief expected 2-3 GB; the real figure is a fifth of that, because ccache compresses and
debug info compresses well. A CI entry is larger than the local figure, and a cap below what one
build produces is worse than no cap: ccache evicts what the same build is about to need. So
`max-size` sits about 1.3x above the size each job's `ccache -s` reported. For Windows and macOS
that is the size of the first build under a cap that no longer bit, on 2026-11-12: the 2.1 and
1.1 GB of the day before were what eviction had left, not what a build produces.

| Job | Key | Entry | `max-size` |
|---|---|---:|---:|
| Windows (MSVC), `pace-cc` | `pace-store-windows-msvc-ci-<Pace version>-<run>` | 335 MB | 1 GB (`--storage-max-disk`) |
| macOS (arm) | `ccache-macosArm-ci` | 0.2 GB | 1500 MB |
| Linux matrix, Clang on x86 | `ccache-ubuntu-24.04-Clang 22-...` | 0.7 GB | 900 MB |
| Packaging for AppImage | `ccache-appimage` | 0.76 GB | 1200 MB |
| `build_no_extensions` | `ccache-ubuntu2404-no-extensions` | 0.55 GB | 700 MB |
| `package_daemon_for_Ubuntu`, 24.04 | `ccache-ubuntu24.04-headless` | 0.33 GB | 500 MB |
| `windows_package`, `macos_package` | none | | |
| Linux matrix: GCC (x86, arm), Clang on arm | none | | |
| `package_daemon_for_Ubuntu`, 26.04 | none | | |
| Packaging, Ubuntu 26.04 | none | | |
| Full-tree clang-tidy (`clang-tidy.yml`) | none | | |

With the production builds' entries the Windows and macOS rows were 2.9 and 1.6 GB, of about 6.8 GB
for one generation of each key, beside roughly 1.7 GB of Qt, vcpkg, FreeBSD, apt and lint-tool
caches: 8.5 GB of the 10 GB quota. The production builds have no entry any more; the plain
configuration's objects carry no link-time-optimization code, and a `Release` build has no debug
information, so both rows shrink. The Pace store held 335 MB after one full plain build, where the
production build's store in the trial was 3.2 GB, and the macOS entry held 0.2 GB where the
production build's held 1.6 GB. The jobs without a
cache run on `master` alone, so they cost no pull request anything; they get their caches back when
the entries shrink.

That headroom is not enough for a second generation, and there is one for a while on every
`master` build: from the moment a job saves its new entry until the prune job has run. In that
window the store is over the quota and GitHub evicts whatever was read least recently, which has
been the FreeBSD image and the apt and lint-tool caches rather than a compiler cache -- a slower
`master` job, not a slower pull request. Smaller entries are what closes the window.

The guard jobs (`package_daemon_for_Ubuntu`, `build_no_extensions`) are not read-only restorers of
the Linux Clang key: they compile with a different preset, so their command lines differ and a
restored Clang-matrix entry would hit almost nothing.

### Windows

The plain Windows job caches its compiles with `pace-cc`, the launcher of
[Pace](https://github.com/LASTRADA-Software/pace), served by a `pace-kv` on the runner whose store
is one file. In a trial on the production build, ccache hit 0.5 to 2 % with a cache at its full size
where `pace-cc` hit 99 %: restoring the 3.2 GB store took 39 s, installing and starting Pace 13 s, and
the 4 minutes of compiles hit 99 %.

The job installs the Pace release named by `PACE_VERSION` at the top of `build.yml` (launcher and
daemon come from the one archive, and must be one version: a wire-version mismatch refuses every
exchange) and checks the archive against the SHA-256 pinned beside it (`PACE_WINDOWS_ZIP_SHA256`;
a mismatch builds without Pace), restores the store, and starts `pace-kv` on it, with its memory tier
capped at 1 GB, through `cmd.exe`, which owns its output
redirection: started by PowerShell with redirected output it died when the step's shell exited. It
then waits until a compile is served, because `cmake/CompileCache.cmake` takes `pace-cc` only when its
own two-second probe of the daemon succeeds and otherwise falls back to the uncached ccache of the
runner image; configure says which (`[cache] Enabling pace-cc` or `[cache] Not using pace-cc ...`), and
the job configures again in a fresh tree, up to three times, while it says the latter. None of the Pace
steps fails the job: without the cache the build is slower, not wrong. After the build it prints
`pace-cc --show-stats`, writes the hits and compiles into the job summary, and stops `pace-kv` before
the store is saved. `pace-kv` has no clean stop on Windows but a console Ctrl-C, which a detached
process does not receive, so it is given 20 seconds to write out what its default
`--storage-durability=batched` holds and then killed; the trial lost nothing that way. Every Pace
step has five minutes, since `continue-on-error` does not end a step that hangs.

A cold store costs about two hours: the first plain run compiled all 1502 translation units (a
median of 14.6 s each) in a build step of 110 minutes, and the job took 127.7. That recurs on the
first `master` run after a `PACE_VERSION` bump and after GitHub evicts the entry, so the job may run
200 minutes.

Whenever a launcher is in use under MSVC, `cmake/CompileCache.cmake` sets
`CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` with `CMAKE_POLICY_DEFAULT_CMP0141=NEW` -- `/Z7`
instead of `/Zi`, which no launcher can cache -- and rewrites a legacy `/Zi` in the flag variables; a
`Release` build has no debug information to begin with.

## What a pull request runs

A pull request runs the platform builds and tests -- `Windows`, `macOSArm` (the plain
configuration, `msvc-ci` and `macos-ci`: build and tests, no signing, no packaging) and one Linux leg
(Clang 22 on x86, built with `-Werror`) -- plus the guard builds, the lint jobs and clang-tidy on the
changed lines. Master pushes and the merge queue run the rest: the production builds of Windows and
macOS with their tests, signing and packages (`Windows (package)`, `macOS (package)`), the other three
compiler legs, packaging, valgrind, conformance, bench-headless, notcurses and FreeBSD. The full-tree
clang-tidy runs nightly (below). A release tag runs everything a master push runs, and the full-tree
clang-tidy as well; the static build runs on master pushes and release
tags, never in the merge queue.

The daemon packages are the two exceptions in kind. `package_daemon_for_Ubuntu` (the Qt-free
`contour-daemon` .deb) runs on every pull request, for 24.04 alone, and for 24.04 and 26.04 otherwise:
it is the only job that builds the GUI-less configuration, so it cannot wait for master. The RPMs
(`package_for_Fedora`: `contour` and `contour-daemon` from one spec, on current Fedora and rawhide) are
ordinary packaging and run on master pushes, the merge queue and release tags only;
rawhide never blocks a release.

A required status check is never gated off pull requests. `tests matrix` and
`Ubuntu Linux 24.04 post-check` always run and treat a skipped dependency as passed (a failed or
cancelled one still fails them). `scripts/check-workflows.py` enforces both rules, and its
`--self-test` pins them against fixtures. A master run is never cancelled and never queued behind
another (its concurrency group is its own commit), so it finishes and saves the compiler cache.

## Nightly runs

A scheduled run has no pull request to turn red, so each nightly workflow ends in a `report` job
that runs `scripts/report-nightly-failure.py`. It keeps one issue per workflow, labelled
`ci-nightly` and titled "Nightly <workflow> is failing":

| Last night | Issue open? | What happens |
|---|---|---|
| failed | no | the issue is opened, with the failed jobs and the lines of their logs that name the failure |
| failed | yes | a comment is added |
| passed | yes | the issue is closed |
| passed | no | nothing |

So GitHub's own issue notification is the alert, and an open `ci-nightly` issue always means
"still failing". With the repository secret `NIGHTLY_ASSIGN_TOKEN` set to a user's token that may
assign issues, a newly opened issue is assigned to the Copilot coding agent; the workflow's own
token is not allowed to do that. Without the secret the issue is simply left unassigned.

## Results

Measured on 2026-10-31 with `scripts/measure-compile-times.py --jobs 16`, the merge base and this
branch built back to back on the same idle machine (Clang 22, RelWithDebInfo), so contention is
equal on both sides:

| | Base (`4de2e780a`) | This branch | Change |
|---|---:|---:|---:|
| Total CPU-s, 1220 TUs | 4832 | 4218 | −12.7 % |
| Frontend CPU-s | 3388 | 2947 | −13.0 % |
| Backend CPU-s | 1360 | 1203 | −11.5 % |

The slowest translation units fall by a quarter to a third: `SettingsController_test.cpp` 34.7 →
23.6 s, `TerminalSessionManager_test.cpp` 30.3 → 22.2 s, `TerminalSessionPermissions_test.cpp`
29.7 → 21.9 s, `Config.cpp` 42.2 → 33.1 s.

What a change to a central header rebuilds (translation units depending on it, `ninja -t deps`):

| Header | Base | This branch |
|---|---:|---:|
| `vthost/SessionHost.hpp` | 115 | 37 |
| `contour/config/Actions.hpp` | 172 | 113 |
| `vtbackend/screen/Terminal.hpp` | 234 | 202 |
| `contour/config/Config.hpp` | 158 | 138 |
| `contour/session/TerminalSession.hpp` | 79 | 67 |
| `contour/window/WindowController.hpp` | 65 | 53 |

The single largest gain is `Action`: a `std::variant` of 129 alternatives whose copy, move and
destruction every translation unit holding an `Action` used to instantiate. It is now a class
deriving from that variant with its special members defined once, in `Actions.cpp`; the variant's
instantiation cost fell by 88 %. Forward declarations alone moved cost between headers more than
they removed it, because the slowest translation units — the GUI tests — genuinely need the
application they drive. `scripts/include-budget.txt` keeps the cuts that were made from coming back.

Pull-request CI, warm cache, a change that touches no central header (wall clock per job):

| Job | Before | After |
|---|---:|---:|
| Linux Clang 22 | 92–107 min | 23.5 min |
| Windows | ~134 min | 31 min |
| macOS | ~125 min | 19–23 min |
| Guard builds (GUI off, no extensions) | 25–60 min | 3–6 min |
| clang-tidy | 160+ min (full tree) | 14 min (changed lines) |

A change that touches a header half the tree includes still rebuilds what depends on it: with this
branch's own header churn the platform jobs took 40–58 min. A cold cache costs what it did before.
