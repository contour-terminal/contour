# contour · feature/out-of-process-extension-host — status

Local-only tracker: excluded via `.git/info/exclude`, never committed. Times are local, from `date`, to the second.

**Last updated:** 2026-11-26 17:05:49

## Current plan: run every extension compiled, in its own sandboxed helper process

Extensions move from the in-process WAMR interpreter to Wasmtime running inside `contour-extension-host`, one sandboxed process per extension, so guest code gets several times faster and a crash or exploit stays inside the extension that caused it. Done means all six phases pass their gates; it lands as one PR on master, which the agent-orchestrator branch rebases onto.

**Decisions:**
- 2026-11-24 23:25:22: Wasmtime in a sandboxed helper (approach C); one process per extension; crash isolation plus containment with OS-level sandboxing on all four platforms.
- 2026-11-24 23:25:22: the helper is named `contour-extension-host`; user-facing text never names the engine.
- 2026-11-24 23:25:22: macOS helper gets `allow-unsigned-executable-memory`; offline distro builds (Fedora, offline Flatpak) and FreeBSD ship WAMR inside the helper.
- 2026-11-24 23:25:22: subagent-driven, per-task review; one subagent and `-j4` niced builds while `contour-workspaces` runs.

| Phase | Tasks done | State | Where it is |
|---|---|---|---|
| 1: Wasmtime in-process behind the contract suite | 7/7 | done | at `19124c9`: 13.7× faster than WAMR in-process; ASan/TSan clean; coverage 96–98 %; 4 WAMR bugs fixed on the way |
| 2: protocol, remote backend, registry and breaker | 8/8 | done | at `3abef4e`: through the helper 18.8 µs per render (2× faster than WAMR); sanitizers clean; coverage 94.9 %; Windows channels verified under wine |
| 3: helper executable, supervisor, lifecycle | 8/8 | done | at `4ec0d13`: one helper process per extension, 18.1–18.6 µs per render (in-process was 18.8), 1.2 ms spawn, 10.8 MiB idle; fix wave re-reviewed clean (3 minors carried into phase 4) |
| 4: shared guest memory | 8/8 | done | at `009b13c`: steady render 8.4 µs (phase 3: 18.1–18.6), copying 18.7 µs, round trip 3.3 µs, spawn 1.28 ms, idle 10.6 MiB, coverage 92.9%; phase review clean after 1 fix wave; macOS/FreeBSD/real Windows await CI |
| 5: sandboxes, WAMR in the helper | 8/9 | running | gate steps pass; phase review done (seam sound); fix wave running on its findings |
| 6: cache, packaging, docs, gates | 0/7 | queued | waits on phase 5 |

**Now:** Pushed 54387ba81 with the Windows and macOS round-5 fixes, both reviewed. CI is running. Things only CI can settle: on Windows, the handle-count fix (wine always reports 0 handles); on macOS, whether the closed-peer check works there.
**Next:** address review findings; push; label the PR for ARM CI; flip platforms whose CI passes with their sandbox; close 5.8; phase 6 (docs, release note, packaging, measurements, final review).

## Time spent

From the time log, stamped as each event happened. *Implement* runs from a task's dispatch to its first commit or report, *review & fixes* from there to done, and *wall time* from the first dispatch to the last completion: less than their sum where work ran in parallel. Work still running counts to 2026-11-26 17:05:49.

| Lane or phase | Finished | Wall time | Implement | Review & fixes | Mean per task | Idle |
|---|---|---|---|---|---|---|
| 1 · Wasmtime in-process | 7 | 2h 55m | 1h 42m | 1h 11m | 24m 58s | — |
| 2 · Protocol and remote backend | 8 | 4h 49m | 1h 54m | 2h 21m | 32m 02s | — |
| 3 · Helper executable and lifecycle | 8 | 6h 25m | 2h 52m | 3h 32m | 48m 12s | — |
| 4 · Shared guest memory | 8 | 4h 58m | 1h 16m | 3h 41m | 37m 15s | — |
| 5 · Sandboxes (running) | 10 | 23h 19m | 4h 20m | 18h 53m | 50m 07s | — |
| **All** (running) | 41 | 41h 39m | 12h 06m | 29h 42m | 39m 25s | — |

### 1 · Wasmtime in-process

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| 1.1 | Pin Wasmtime, verify its C API | 2026-11-24 23:26:04 | 3m 16s | 2m 11s | 5m 27s | — | 0 |
| 1.2 | Engine feature table enforced by both engines | 2026-11-24 23:31:31 | 9m 13s | 2m 19s | 11m 32s | — | 0 |
| 1.3 | WasmtimeBackend: runtimes, modules, linkage, generations | 2026-11-24 23:43:03 | 5m 20s | 19m 30s | 24m 50s | — | 1 |
| 1.4 | Instances, calls, host functions, memory, limits | 2026-11-25 00:07:53 | 4m 04s | 17m 04s | 21m 08s | — | 0 |
| 1.5 | Our WASI preview-1 shim, shared by every engine | 2026-11-25 00:29:01 | 16m 14s | 2m 57s | 19m 11s | — | 0 |
| 1.6 | Sticky interruption by epoch, no ticker | 2026-11-25 00:48:13 | 4m 16s | 27m 57s | 32m 13s | — | 1 |
| 1.7 | Phase 1 gate: sanitizers, speed-up, review | 2026-11-25 01:20:49 | 1h 00m | — | 1h 00m | — | 2 |

**Task 1.3 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: signal handlers in tests, system probe, WASI signature test | 2026-11-25 00:02:50 | 2026-11-25 00:06:27 | 2026-11-25 00:06:27 | 2026-11-25 00:07:10 | 4m 20s | 3 | 3 Important + 2 Minor fixed in 3 commits |
| review2 | Scoped re-review of fix round 1 | 2026-11-25 00:07:10 | — | — | 2026-11-25 00:07:53 | 0m 43s | 0 | all addressed |

2 steps, 3 commits · wall time 5m 03s · step time 5m 03s.

**Task 1.6 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: absorbed nested interrupt, TSan in tests, WAMR fault restore | 2026-11-25 01:14:22 | 2026-11-25 01:18:14 | 2026-11-25 01:18:14 | 2026-11-25 01:18:39 | 4m 17s | 2 | 5 items fixed, TSan 0 reports |
| review2 | Scoped re-review of fix round 1 | 2026-11-25 01:18:39 | — | — | 2026-11-25 01:20:26 | 1m 47s | 0 | all addressed |

2 steps, 2 commits · wall time 6m 04s · step time 6m 04s.

**Task 1.7 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| gate | Sanitizer runs and WAMR vs Wasmtime measurement | 2026-11-25 01:20:49 | 2026-11-25 01:24:32 | 2026-11-25 01:24:32 | 2026-11-25 01:31:45 | 10m 56s | 1 | WAMR 38.3us vs Wasmtime 2.8us per render (13.7x; 18.5x full redraw); ASan/TSan clean; coverage 96%/98% |
| review | Phase 1 whole-diff code review | 2026-11-25 01:31:45 | — | — | 2026-11-25 01:47:41 | 15m 56s | 0 | 1 Important (unbounded table growth on Wasmtime), 8 Minor |
| fixwave | Fix wave for the phase 1 review | 2026-11-25 01:47:41 | 2026-11-25 01:59:35 | 2026-11-25 02:09:10 | 2026-11-25 02:12:55 | 25m 14s | 5 | 9 findings fixed in 5 commits |
| rereview | Re-review of the phase 1 fix wave | 2026-11-25 02:12:55 | — | — | 2026-11-25 02:15:24 | 2m 29s | 0 | 8 addressed + 1 addressed with new Important I1 (rewrite drops trailing bytes) |
| fix2 | Fix: table-section trailing bytes, single parse | 2026-11-25 02:15:24 | 2026-11-25 02:19:39 | 2026-11-25 02:19:39 | 2026-11-25 02:20:20 | 4m 56s | 1 | one commit; >1 table refused at load on both engines |
| rereview2 | Re-review of the table fix | 2026-11-25 02:20:21 | — | — | 2026-11-25 02:21:15 | 0m 54s | 0 | all addressed |

6 steps, 7 commits · wall time 1h 00m · step time 1h 00m · 0m 01s with no step running.

### 2 · Protocol and remote backend

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| 2.0 | Expand phase 2 to step level | 2026-11-25 01:32:11 | 15m 30s | — | 15m 30s | — | 0 |
| 2.1 | Wire protocol, codec, channels, framed streams | 2026-11-25 02:21:15 | 16m 11s | 22m 52s | 39m 03s | — | 2 |
| 2.4 | Contract changes at the seam | 2026-11-25 03:00:18 | 6m 08s | 1m 38s | 7m 46s | — | 0 |
| 2.2 | IHostedEngine and HostServer | 2026-11-25 03:08:04 | 3m 35s | 32m 04s | 35m 39s | — | 1 |
| 2.3 | RemoteBackend over an in-process HostServer | 2026-11-25 03:43:43 | 2m 08s | 32m 11s | 34m 19s | — | 1 |
| 2.5 | makeDefaultBackend(BackendConfig) at every composition root | 2026-11-25 04:18:03 | 3m 23s | 42m 56s | 46m 19s | — | 2 |
| 2.6 | ExtensionRegistry and the per-extension breaker | 2026-11-25 05:04:22 | 26m 09s | 10m 16s | 36m 25s | — | 1 |
| 2.7 | Phase 2 gate | 2026-11-25 05:40:47 | 41m 17s | — | 41m 17s | — | 1 |

**Task 2.1 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: element caps, Windows shutdown wakes peer | 2026-11-25 02:41:42 | 2026-11-25 02:48:16 | 2026-11-25 02:48:16 | 2026-11-25 02:48:59 | 7m 17s | 1 | 6 items fixed in one commit |
| review2 | Scoped re-review of fix round 1 | 2026-11-25 02:48:59 | — | — | 2026-11-25 02:50:54 | 1m 55s | 0 | 6 addressed; new Important N1 (Windows CancelIoEx after close race) |
| fix2 | Fix round 2: Windows shutdown race, encoder caps | 2026-11-25 02:50:54 | 2026-11-25 02:58:05 | 2026-11-25 02:58:05 | 2026-11-25 02:58:38 | 7m 44s | 1 | N1/N2 + encoder caps |
| review3 | Scoped re-review of fix round 2 | 2026-11-25 02:58:38 | — | — | 2026-11-25 03:00:18 | 1m 40s | 0 | all addressed |

4 steps, 2 commits · wall time 18m 36s · step time 18m 36s.

**Task 2.2 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: nesting bound, executor stacks, busy-lane test | 2026-11-25 03:33:56 | 2026-11-25 03:40:50 | 2026-11-25 03:40:50 | 2026-11-25 03:41:28 | 7m 32s | 1 | 7 items, one commit |
| review2 | Scoped re-review of fix round 1 | 2026-11-25 03:41:28 | — | — | 2026-11-25 03:43:43 | 2m 15s | 0 | all addressed |

2 steps, 1 commit · wall time 9m 47s · step time 9m 47s.

**Task 2.3 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: respawn mismatch, stale cache hit, validation tests | 2026-11-25 04:11:35 | 2026-11-25 04:16:04 | 2026-11-25 04:16:04 | 2026-11-25 04:16:32 | 4m 57s | 1 | 6 items, one commit |
| review2 | Scoped re-review of fix round 1 | 2026-11-25 04:16:32 | — | — | 2026-11-25 04:18:02 | 1m 30s | 0 | all addressed |

2 steps, 1 commit · wall time 6m 27s · step time 6m 27s.

**Task 2.5 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: page-shadow memory cache, engine-neutral hooks | 2026-11-25 04:47:52 | 2026-11-25 04:58:42 | 2026-11-25 04:59:04 | 2026-11-25 04:59:44 | 11m 52s | 3 | page shadow; hooks refused engine-neutrally |
| review2 | Scoped re-review of fix round 1 | 2026-11-25 04:59:44 | — | — | 2026-11-25 05:01:48 | 2m 04s | 0 | 1-3 addressed; snapshot full-size contradicts R16 |
| fix2 | Fix round 2: per-page snapshots | 2026-11-25 05:01:49 | 2026-11-25 05:04:00 | 2026-11-25 05:04:00 | 2026-11-25 05:04:22 | 2m 33s | 2 | per-page snapshots; controller-checked |

3 steps, 5 commits · wall time 16m 30s · step time 16m 29s · 0m 01s with no step running.

**Task 2.6 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: gated shared-load test, comments, digest memo | 2026-11-25 05:34:47 | 2026-11-25 05:40:14 | 2026-11-25 05:40:14 | 2026-11-25 05:40:47 | 6m 00s | 1 | gated shared-load tests; controller-checked |

1 step, 1 commit · wall time 6m 00s · step time 6m 00s.

**Task 2.7 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| gate | Sanitizers, IPC overhead, Windows under wine, coverage | 2026-11-25 05:40:48 | 2026-11-25 05:45:19 | 2026-11-25 05:50:51 | 2026-11-25 05:51:54 | 11m 06s | 2 | remote render 18.8us steady / 29.1us uncached vs 2.8/12.4 in-process; round trip 3.3us; sanitizers clean; Windows transport 22/22 under wine (fixed a NOMINMAX build break); coverage 94.9% |
| review | Phase 2 whole-diff code review | 2026-11-25 05:41:12 | — | — | 2026-11-25 05:57:28 | 16m 16s | 0 | 1 Important (host death during memory read counted as guest trap), 6 Minor |
| fixwave | Fix wave for the phase 2 review | 2026-11-25 05:57:28 | 2026-11-25 06:03:03 | 2026-11-25 06:12:52 | 2026-11-25 06:19:11 | 21m 43s | 7 | 7 findings, 7 commits |
| rereview | Re-review of the phase 2 fix wave | 2026-11-25 06:19:11 | — | — | 2026-11-25 06:22:04 | 2m 53s | 0 | all addressed |

4 steps, 9 commits · wall time 41m 16s · step time 51m 58s · 10m 42s of it in parallel.

### 3 · Helper executable and lifecycle

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| 3.0 | Expand phase 3 to step level | 2026-11-25 06:22:04 | 25m 48s | — | 25m 48s | — | 0 |
| 3.1 | Process launcher, handles, inherited handles | 2026-11-25 06:47:52 | 25m 25s | 15m 45s | 41m 10s | — | 1 |
| 3.2 | The contour-extension-host executable | 2026-11-25 07:29:02 | 1m 31s | 27m 14s | 28m 45s | — | 1 |
| 3.3 | Supervisor, process connector, non-call timeouts | 2026-11-25 07:57:47 | 1m 35s | 46m 27s | 48m 02s | — | 1 |
| 3.4 | Cross-process interruption and the backstop | 2026-11-25 08:45:49 | 0m 52s | 1h 04m | 1h 04m | — | 1 |
| 3.5 | Teardown off the owner thread | 2026-11-25 09:50:48 | 0m 51s | 40m 26s | 41m 17s | — | 1 |
| 3.6 | Crash and hostility suite | 2026-11-25 10:32:05 | 17m 24s | 18m 57s | 36m 21s | — | 1 |
| 3.7 | Phase 3 gate | 2026-11-25 11:08:27 | 1h 39m | — | 1h 39m | — | 1 |

**Task 3.1 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: Windows inheritance window, macOS exit race, read-only control page | 2026-11-25 07:20:49 | 2026-11-25 07:26:33 | 2026-11-25 07:26:33 | 2026-11-25 07:27:02 | 6m 13s | 1 | 9 items, one commit |
| review2 | Scoped re-review of fix round 1 | 2026-11-25 07:27:02 | — | — | 2026-11-25 07:29:02 | 2m 00s | 0 | all addressed |

2 steps, 1 commit · wall time 8m 13s · step time 8m 13s.

**Task 3.2 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: assert the real helper's caps, CMake bundle dependency | 2026-11-25 07:51:38 | 2026-11-25 07:56:31 | 2026-11-25 07:56:31 | 2026-11-25 07:56:42 | 5m 04s | 1 | 6 items; caps proven in release |
| review2 | Scoped re-review | 2026-11-25 07:56:42 | — | — | 2026-11-25 07:57:47 | 1m 05s | 0 | all addressed |

2 steps, 1 commit · wall time 6m 09s · step time 6m 09s.

**Task 3.3 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: hanging tests, clock refresh, kill accounting | 2026-11-25 08:39:08 | 2026-11-25 08:44:02 | 2026-11-25 08:44:02 | 2026-11-25 08:44:11 | 5m 03s | 1 | 7 items; hangs now fail |
| review2 | Scoped re-review | 2026-11-25 08:44:11 | — | — | 2026-11-25 08:45:49 | 1m 38s | 0 | all addressed |

2 steps, 1 commit · wall time 6m 41s · step time 6m 41s.

**Task 3.4 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: arm-the-right-frame race, fetch re-check, sanitizer check on real-guest suites | 2026-11-25 09:33:37 | 2026-11-25 09:47:48 | 2026-11-25 09:47:48 | 2026-11-25 09:48:29 | 14m 52s | 3 | 6 items in 3 commits |
| review2 | Scoped re-review | 2026-11-25 09:48:29 | — | — | 2026-11-25 09:50:48 | 2m 19s | 0 | all addressed |

2 steps, 3 commits · wall time 17m 11s · step time 17m 11s.

**Task 3.5 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: lost-wakeup release, interface doc | 2026-11-25 10:26:51 | 2026-11-25 10:30:50 | 2026-11-25 10:30:50 | 2026-11-25 10:31:02 | 4m 11s | 1 | 5 items |
| review2 | Scoped re-review | 2026-11-25 10:31:02 | — | — | 2026-11-25 10:32:04 | 1m 02s | 0 | all addressed |

2 steps, 1 commit · wall time 5m 13s · step time 5m 13s.

**Task 3.6 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: per-row kill reasons so every row discriminates | 2026-11-25 10:58:00 | 2026-11-25 11:05:54 | 2026-11-25 11:05:54 | 2026-11-25 11:06:30 | 8m 30s | 2 | 6 items, mutations bite |
| review2 | Scoped re-review | 2026-11-25 11:06:31 | — | — | 2026-11-25 11:08:26 | 1m 55s | 0 | all addressed |

2 steps, 2 commits · wall time 10m 26s · step time 10m 25s · 0m 01s with no step running.

**Task 3.7 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| gate | Gate checks: sanitizers, real-helper suites, measurement, wine, coverage | 2026-11-25 11:08:27 | 2026-11-25 11:10:49 | 2026-11-25 11:25:46 | 2026-11-25 11:30:27 | 22m 00s | 3 | green; render 18.4 µs steady via real helper |
| review | Whole-phase code review | 2026-11-25 11:30:27 | — | — | 2026-11-25 11:47:14 | 16m 47s | 0 | 1 Critical (Apple Silicon shm size), 4 Important |
| fixwave | Fix wave: Apple Silicon, pre-thread confinement + no core dumps, setsid, logging | 2026-11-25 11:47:14 | 2026-11-25 11:54:45 | 2026-11-25 12:19:15 | 2026-11-25 12:43:30 | 56m 16s | 6 | 9 items in 6 commits; ASan/TSan/release/wine green |
| fixreview | Scoped review of the fix wave | 2026-11-25 12:43:30 | — | — | 2026-11-25 12:47:38 | 4m 08s | 0 | all 9 ✅; 0 Critical/Important; 3 Minor carried to phase 4 |

4 steps, 9 commits · wall time 1h 39m · step time 1h 39m.

### 4 · Shared guest memory

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| 4.0 | Expand phase 4 to step level | 2026-11-25 12:47:38 | 19m 28s | — | 19m 28s | — | 0 |
| 4.1 | Carried minors | 2026-11-25 13:07:06 | 2m 21s | 16m 04s | 18m 25s | — | 0 |
| 4.2 | Shared objects, one creation path | 2026-11-25 13:25:31 | 5m 53s | 17m 41s | 23m 34s | — | 0 |
| 4.3 | Handing an object to the helper; protocol v3 | 2026-11-25 13:49:05 | 2m 44s | 36m 06s | 38m 50s | — | 1 |
| 4.4 | Helper side: mapping, memory creator, growth through the parent | 2026-11-25 14:27:55 | 23m 10s | 19m 33s | 42m 43s | — | 1 |
| 4.5 | Parent side: an object per instance, acknowledged size, answered growth | 2026-11-25 15:10:38 | 6m 07s | 1h 00m | 1h 06m | — | 1 |
| 4.6 | Single-fetch decoding | 2026-11-25 16:17:37 | 10m 28s | 21m 11s | 31m 39s | — | 0 |
| 4.7 | Phase 4 gate | 2026-11-25 16:49:49 | 6m 20s | 50m 03s | 56m 23s | — | 1 |

**Task 4.3 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: rejected descriptors Malformed, forged-handle test, release-by-default claim | 2026-11-25 14:20:22 | 2026-11-25 14:26:18 | 2026-11-25 14:26:18 | 2026-11-25 14:26:48 | 6m 26s | 2 | 6 findings fixed in 2 commits |
| rereview | Re-review of fix round 1 | 2026-11-25 14:26:48 | — | — | 2026-11-25 14:27:55 | 1m 07s | 0 | all fixed; no new issues |

2 steps, 2 commits · wall time 7m 33s · step time 7m 33s.

**Task 4.4 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: FreeBSD flag, grow fault, Signals test in release, dependency | 2026-11-25 14:57:18 | 2026-11-25 15:08:03 | 2026-11-25 15:08:03 | 2026-11-25 15:08:46 | 11m 28s | 4 | 6 fixed in 4 commits; Signals path runs clean in release |
| rereview | Re-review of fix round 1 | 2026-11-25 15:08:46 | — | — | 2026-11-25 15:10:38 | 1m 52s | 0 | all fixed; 1 Low + nit carried |

2 steps, 4 commits · wall time 13m 20s · step time 13m 20s.

**Task 4.5 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: bad growth during setup tests | 2026-11-25 16:10:16 | 2026-11-25 16:16:49 | 2026-11-25 16:16:49 | 2026-11-25 16:17:36 | 7m 20s | 1 | addressed; re-review clean |

1 step, 1 commit · wall time 7m 20s · step time 7m 20s.

**Task 4.7 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| review | Phase review of phase 4 | 2026-11-25 17:21:30 | — | — | 2026-11-25 17:33:21 | 11m 51s | 0 | 1 Important (descriptor sent up can hang a parent thread on close), 2 minors |
| fix1 | Phase review fixes | 2026-11-25 17:33:21 | 2026-11-25 17:39:10 | 2026-11-25 17:41:44 | 2026-11-25 17:46:12 | 12m 51s | 3 | 3 fixed; re-review clean |

2 steps, 3 commits · wall time 24m 42s · step time 24m 42s.

### 5 · Sandboxes

| Task | What | Started | Implement | Review & fixes | Total | Idle | Fix rounds |
|---|---|---|---|---|---|---|---|
| 5.0 | Expand phase 5 to step level | 2026-11-25 17:46:39 | 12m 52s | — | 12m 52s | — | 0 |
| 5.1 | Sandbox seam and lockdown order | 2026-11-25 17:59:38 | 29m 13s | 11m 00s | 40m 13s | — | 1 |
| 5.2 | Linux sandbox: seccomp, no_new_privs, Landlock | 2026-11-25 18:39:51 | 5m 32s | 1h 09m | 1h 14m | — | 2 |
| 5.3 | Interpreter engine in the helper | 2026-11-25 19:55:32 | 6m 18s | 1h 04m | 1h 10m | — | 1 |
| 5.ci1 | CI fixes for the draft PR | 2026-11-25 21:06:57 | 9m 02s | — | 9m 02s | — | 0 |
| 5.4 | Linux runs extensions out of process by default | 2026-11-25 21:16:40 | 5m 57s | 1h 12m | 1h 18m | — | 2 |
| 5.5 | FreeBSD sandbox (dormant) | 2026-11-25 22:36:02 | 28m 30s | 48m 42s | 1h 17m | — | 1 |
| 5.6 | macOS sandbox and the helper's entitlement | 2026-11-25 23:53:52 | 1m 21s | 42m 31s | 43m 52s | — | 1 |
| 5.7 | Windows sandbox (AppContainer) | 2026-11-26 00:38:23 | 23m 07s | 40m 22s | 1h 03m | — | 2 |
| 5.ci3 | CI fixes: Windows duplicate engine symbols, interpreter helper on debug builds | 2026-11-26 01:41:52 | 21m 29s | 8m 40s | 30m 09s | — | 0 |
| 5.8 | Phase 5 gate (running) | 2026-11-26 02:12:37 | 1h 57m | 12h 55m | 14h 53m | — | — |

**Task 5.1 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: re-split commits so each builds | 2026-11-25 18:34:16 | 2026-11-25 18:36:37 | 2026-11-25 18:38:04 | 2026-11-25 18:39:51 | 5m 35s | 3 | re-split; tree identical; each commit builds |

1 step, 3 commits · wall time 5m 35s · step time 5m 35s.

**Task 5.2 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: sanitizer exit, Landlock ABI fallback | 2026-11-25 19:45:18 | 2026-11-25 19:47:25 | 2026-11-25 19:49:43 | 2026-11-25 19:50:56 | 5m 38s | 2 | 2 commits: sanitizer leak-check off in the helper; Landlock steps down an ABI on EINVAL |
| rereview1 | Re-review of fix round 1 | 2026-11-25 19:50:56 | — | — | 2026-11-25 19:53:00 | 2m 04s | 0 | 2 addressed; 1 new Important (test env drops SystemRoot on Windows) |
| fix2 | Fix round 2: helper test keeps the default environment | 2026-11-25 19:53:00 | 2026-11-25 19:49:43 | 2026-11-25 19:49:43 | 2026-11-25 19:54:50 | 1m 50s | 1 | SystemRoot kept; folded by autosquash; wine passes |

3 steps, 3 commits · wall time 9m 32s · step time 9m 32s.

**Task 5.3 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: CPU clock call, JIT-capable WAMR, Fedora packaging | 2026-11-25 20:50:45 | 2026-11-25 20:59:05 | 2026-11-25 20:59:05 | 2026-11-25 21:02:01 | 11m 16s | 3 | clocks withheld alike; JIT WAMR refused; Fedora packages helper |
| rereview1 | Re-review of fix round 1 | 2026-11-25 21:02:02 | — | — | 2026-11-25 21:06:20 | 4m 18s | 0 | all 3 addressed |

2 steps, 3 commits · wall time 15m 35s · step time 15m 34s · 0m 01s with no step running.

**Task 5.4 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1 + CI fixes 2: .deb helper, AppImage, tidy, MSVC, fork lint | 2026-11-25 22:07:47 | — | — | 2026-11-25 22:26:30 | 18m 43s | 0 | deb/AppImage helper, tidy, MSVC, fork lint; pushed 4647dde |
| rereview1 | Re-review of fix round 1 | 2026-11-25 22:26:30 | — | — | 2026-11-25 22:29:31 | 3m 01s | 0 | 6 of 7 addressed; AppImage flag wrong |
| fix2 | Fix round 2: AppImage helper patched in place | 2026-11-25 22:29:31 | 2026-11-25 21:56:00 | 2026-11-25 21:56:00 | 2026-11-25 22:35:17 | 5m 46s | 1 | AppImage patched in place; pushed 98ebe9f |

3 steps, 1 commit · wall time 27m 30s · step time 27m 30s.

**Task 5.5 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: address-space cap, rights tests, unlimited descriptors | 2026-11-25 23:20:01 | 2026-11-25 23:05:12 | 2026-11-25 23:05:12 | 2026-11-25 23:47:42 | 27m 41s | 1 | cap doubled + headroom; every descriptor's rights tested |
| rereview1 | Re-review of fix round 1 | 2026-11-25 23:47:43 | — | — | 2026-11-25 23:53:14 | 5m 31s | 0 | all addressed |

2 steps, 1 commit · wall time 33m 13s · step time 33m 12s · 0m 01s with no step running.

**Task 5.6 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: entitlement proof, profile allow-list test, deny log | 2026-11-26 00:21:40 | — | — | 2026-11-26 00:33:36 | 11m 56s | 0 | airtight entitlement proof, exact allow-list test, filtered deny log, GCC fix; pushed 9fb1290 |
| rereview1 | Re-review of fix round 1 | 2026-11-26 00:33:36 | — | — | 2026-11-26 00:37:44 | 4m 08s | 0 | all addressed |

2 steps, 0 commits · wall time 16m 04s · step time 16m 04s.

**Task 5.7 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| fix1 | Fix round 1: diagnosable CI, non-vacuous ACL and containment tests | 2026-11-26 01:11:50 | — | — | 2026-11-26 01:28:07 | 16m 17s | 0 | diagnosable CI, non-vacuous tests, grant first |
| rereview1 | Re-review of fix round 1 | 2026-11-26 01:28:07 | — | — | 2026-11-26 01:31:48 | 3m 41s | 0 | all addressed; minors for diagnosability |
| fix2 | Fix round 2: diagnosability minors | 2026-11-26 01:31:49 | — | — | 2026-11-26 01:41:51 | 10m 02s | 0 | stderr after reap, NO_PROPAGATE, accepting listener, NotGranted |

3 steps, 0 commits · wall time 30m 01s · step time 30m 00s · 0m 01s with no step running.

**Task 5.8 by step**

| Step | What | Started | First commit | Last commit | Done | Took | Commits | Note |
|---|---|---|---|---|---|---|---|---|
| review | Phase review of phase 5 | 2026-11-26 02:52:49 | — | — | 2026-11-26 03:07:51 | 15m 02s | 0 | 3 Important, 6 Minor; fix wave dispatched |
| fixwave | Phase review fix wave | 2026-11-26 03:07:51 | — | — | 2026-11-26 04:09:54 | 1h 02m | 0 | 9 items done, head 75eb4d4a7; second WAMR race left for a ticket |
| fixwave-review | Review of the fix wave | 2026-11-26 04:09:54 | — | — | 2026-11-26 04:16:56 | 7m 02s | 0 | spec 9/9, 1 Important (pre-existing WAMR race), 7 minors |
| fixwave-fix1 | Fix round 1 of the fix-wave review | 2026-11-26 04:16:56 | — | — | 2026-11-26 04:31:16 | 14m 20s | 0 | head 4491235ab; WAMR patch for cluster-list race |
| fixwave-review2 | Re-review of fix round 1 | 2026-11-26 04:31:16 | — | — | 2026-11-26 04:35:50 | 4m 34s | 0 | 8/8; 1 Important (priming OOM unreachable) |
| fixwave-fix2 | Fix round 2 | 2026-11-26 04:35:50 | — | — | 2026-11-26 04:45:06 | 9m 16s | 0 | head 790ce3259 |
| fixwave-review3 | Re-review of fix round 2 | 2026-11-26 04:45:06 | — | — | 2026-11-26 04:48:28 | 3m 22s | 0 | 1 Critical: patch skipped on BSD patch |
| fixwave-fix3 | Fix round 3 | 2026-11-26 04:48:28 | — | — | 2026-11-26 04:55:08 | 6m 40s | 0 | head 9aefa2abe |
| fixwave-review4 | Re-review of fix round 3 | 2026-11-26 04:55:08 | — | — | 2026-11-26 05:00:00 | 4m 52s | 0 | clean; pushed 9aefa2abe |
| windows-fix | Fix the first real Windows CI failures | 2026-11-26 05:00:00 | — | — | 2026-11-26 05:22:06 | 22m 06s | 0 | trap test fixed; AppContainer refusal instrumented; pushed 8484a0694 |
| windows-review | Review of the Windows CI fix | 2026-11-26 05:22:06 | — | — | 2026-11-26 05:27:01 | 4m 55s | 0 | clean, 5 minors |
| ci2-fix | Fix Windows env block + ARM HostServer case | 2026-11-26 07:20:18 | — | — | 2026-11-26 07:41:11 | 20m 53s | 0 | head b5732de14 pushed |
| ci2-review | Review of the CI round 2 fix | 2026-11-26 07:41:11 | — | — | 2026-11-26 07:44:12 | 3m 01s | 0 | 1 Important: contained env carries parent's paths |
| ci2-fix1 | Contained env from the container folder | 2026-11-26 07:44:12 | — | — | 2026-11-26 07:54:53 | 10m 41s | 0 | 785859fbf pushed |
| ci2-review2 | Re-review of the container-folder environment | 2026-11-26 07:54:53 | — | — | 2026-11-26 08:02:19 | 7m 26s | 0 | 1 Important: prefix check |
| ci2-fix2 | Exact container-folder checks | 2026-11-26 08:02:19 | — | — | 2026-11-26 08:02:19 | 0m 00s | 0 | ee4d5ba28 |
| ci2-review3 | Re-review of the exact checks | 2026-11-26 08:02:19 | — | — | 2026-11-26 08:04:22 | 2m 03s | 0 | clean |
| ci3 | CI round 3: Windows contained DLL init failure | 2026-11-26 09:58:04 | — | — | 2026-11-26 10:06:07 | 8m 03s | 0 | ole32 delay-loaded (user32 via window station); import guard test; minors 1,3; exit code named |
| ci3-review | Review CI round 3 | 2026-11-26 10:06:08 | — | — | 2026-11-26 10:09:11 | 3m 03s | 0 | spec pass; 1 Important (LNK4199), 4 minors |
| ci3-fix1 | Fix round 1: LNK4199 + minors | 2026-11-26 10:09:11 | — | — | 2026-11-26 10:17:10 | 7m 59s | 0 | run-time lookup replaces delay-load; 4 minors done |
| ci3-review2 | Re-review fix round 1 | 2026-11-26 10:17:10 | — | — | 2026-11-26 10:19:06 | 1m 56s | 0 | clean, 4 minors; pushed c2d8d12e5 |
| ci3-fix2 | Round 2: lost exit status kind + doc | 2026-11-26 10:19:06 | — | — | 2026-11-26 10:22:53 | 3m 47s | 0 | ExitKind::Lost; doc clause |
| ci3-review3 | Re-review round 2 | 2026-11-26 10:22:53 | — | — | 2026-11-26 10:23:41 | 0m 48s | 0 | pass, 1 minor |
| ci4-windows | CI round 4: Windows DLL init diagnosis | 2026-11-26 13:08:12 | — | — | 2026-11-26 13:19:43 | 11m 31s | 0 | DETACHED_PROCESS + loader probe diagnostic |
| ci4-windows-review | Review Windows round 4 | 2026-11-26 13:19:43 | — | — | 2026-11-26 13:23:03 | 3m 20s | 0 | 2 Important, 7 minor |
| ci4-windows-fix1 | Windows round 4 fixes | 2026-11-26 13:23:03 | — | — | 2026-11-26 13:27:08 | 4m 05s | 0 | 2 Important + minors fixed |
| ci4-windows-review2 | Re-review Windows round 4 fixes | 2026-11-26 13:27:08 | — | — | 2026-11-26 13:29:02 | 1m 54s | 0 | pass, 3 minors folded |
| ci4-macos | CI round 4: macOS sandbox failures | 2026-11-26 13:30:23 | — | — | 2026-11-26 13:52:28 | 22m 05s | 0 | 4 causes found, 5 commits |
| ci4-macos-review | Review macOS round 4 | 2026-11-26 13:52:28 | — | — | 2026-11-26 13:57:36 | 5m 08s | 0 | pass, 1 Important (unasserted net defences), 5 minor |
| ci4-macos-fix1 | macOS round 4 fixes | 2026-11-26 13:57:36 | — | — | 2026-11-26 14:06:04 | 8m 28s | 0 | 6 items fixed |
| ci4-macos-review2 | Re-review macOS round 4 fixes | 2026-11-26 14:06:05 | — | — | 2026-11-26 14:09:50 | 3m 45s | 0 | pass, 3 minors; pushed a954a9112 |
| ci5-wait | CI on a954a9112 | 2026-11-26 14:09:51 | — | — | 2026-11-26 16:22:56 | 2h 13m | 0 | Windows contained launch works; 4 Windows + 2 macOS failures left |
| ci5-fix-windows | CI round 5 fixes: Windows | 2026-11-26 16:22:56 | — | — | 2026-11-26 16:43:01 | 20m 05s | 0 | 4 fixed; 2 root causes differed (doubled LOCALAPPDATA, spawn 367); head c47cf3330 |
| ci5-windows-review | Review CI round 5 Windows fixes | 2026-11-26 16:43:01 | — | — | 2026-11-26 16:46:15 | 3m 14s | 0 | spec pass; 2 Important, 6 minor |
| ci5-windows-fix1 | Fix round 1: Windows review findings | 2026-11-26 16:46:15 | — | — | 2026-11-26 16:51:41 | 5m 26s | 0 | 8 fixed; head d403214fa |
| ci5-windows-review2 | Re-review Windows fix round 1 | 2026-11-26 16:51:41 | — | — | 2026-11-26 16:52:42 | 1m 01s | 0 | 7/8; #1 partial + 1 minor |
| ci5-windows-fix2 | Fix round 2: handle-count test start barrier | 2026-11-26 16:52:42 | — | — | 2026-11-26 16:54:56 | 2m 14s | 0 | head 9c87b6d5d; controller-checked |
| ci5-fix-macos | CI round 5 fixes: macOS | 2026-11-26 16:54:56 | — | — | 2026-11-26 16:59:56 | 5m 00s | 0 | send polls POLLHUP on macOS; head 3d8a32406 |
| ci5-macos-review | Review CI round 5 macOS fix | 2026-11-26 16:59:56 | — | — | 2026-11-26 17:02:59 | 3m 03s | 0 | pass; 3 minors |
| ci5-macos-fix1 | Fix round 1: macOS minors | 2026-11-26 17:02:59 | — | — | 2026-11-26 17:05:05 | 2m 06s | 0 | 3 minors; test misplaced under __linux__ |
| ci5-macos-fix2 | Fix round 2: run the handle test on macOS | 2026-11-26 17:05:05 | — | — | 2026-11-26 17:05:49 | 0m 44s | 0 | pushed 54387ba81 |
| ci6-wait | CI on 54387ba81 (running) | 2026-11-26 17:05:49 | — | — | — | 0m 01s | 0 |  |

42 steps, 0 commits · wall time 14h 13m · step time 7h 40m · 6h 32m with no step running.

## Key rulings

- 2026-11-25 17:59:56: phase 5 plan ruled. A failed lockdown keeps a helper refused until restart (not relaunched per extension). The WAMR helper copies memory unless a one-hour spike finds a way to share it. Without CI, only Linux switches to out-of-process by default; macOS, FreeBSD and Windows sandbox code lands dormant. Residual risks accepted provisionally (owner may overrule): file metadata stays visible via stat; seccomp alone where Landlock is missing; no parent-death binding on macOS; FreeBSD threads not capped.

- 2026-11-25 13:07:24: phase 4 rulings. A start function may grow memory, routed through Contour on the control channel, so behaviour matches in-process (R29). A helper's live shared memory is capped at the 1 GiB data cap (R30). The gate requires no slowdown against phase 3 rather than a fixed speed-up (R31).

- 2026-11-25 11:30:45: the helper turns off core dumps as part of its confinement — a crash must not stall in a dumper or write extension memory to disk.
- 2026-11-25 06:48:08: phase 3 design accepted, including: lane I/O stays blocking but is always bounded by the interrupt, the grace timer and a kill; the helper applies its own resource limits before it reads any extension code; strict sandbox requirement by default.
- 2026-11-25 05:51:54: phase 2 measured: going through the helper costs ~16 µs per render (18.8 µs vs 2.8 µs in-process) — still 2× faster than WAMR today; phase 4's shared memory targets most of that cost.
- 2026-11-25 04:47:52: the copy-path guest-memory cache is replaced by a page-level shadow of guest memory — overlapping views are then the same bytes, ending a run of three overlap bugs.
- 2026-11-25 03:33:56: the per-extension table cap travels to the helper in the runtime-creation message (from 2.3); a start function that never returns is accepted as un-interruptible in-process — the phase 3 timeout kills the helper.
- 2026-11-25 01:32:11: phase 1 measured: Wasmtime renders `plasma` 13.7× faster than WAMR (18.5× on full redraws); compile costs ~58 ms per module, which the phase 2 registry (load once) and the phase 6 cache absorb.
- 2026-11-25 01:14:22: a nanosecond-wide lost-wakeup window in Wasmtime's epoch callback is accepted in-process (that backend is test-only); in production the supervisor kills a helper whose interrupted call does not return.
- 2026-11-24 23:41:37: extended-const is allowed: Wasmtime 48 cannot switch it off, so WAMR enables it too and both engines accept the same modules.
- 2026-11-24 23:25:22: pre-flight R1–R5 (ledger): guard UB in the feature test; WASI signature table created in 1.3 with trapping stubs until 1.5; epoch deadline at maximum until 1.6; each engine measures in its own test binary; `-j4` niced builds while the peer session runs.

## Follow-ups

- macOS, FreeBSD and real Windows are checked only by reading and wine; draft PR #2431 now runs them in CI. What CI must show: macOS compiles (one size calculation may not count as a compile-time constant there); the descriptor-to-message tests pass on macOS/FreeBSD; Windows placeholder mappings and cross-view commits work on a real machine.
- WAMR does not cross-build for Windows with clang-cl (its build picks the Linux platform; an upstream comment trips clang). Pre-existing, unrelated to this work; worth a ticket.
- Reloading the configuration resets the crash breakers, but a pane whose extension already disabled itself only comes back when it is rebuilt (unchanged behaviour from master). Worth a separate change: re-enable disabled extensions on reload.
- A real probe for a system-installed Wasmtime (upstream ships no CMake config): planned into packaging (6.3).
