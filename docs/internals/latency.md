# Latency: what the daemon costs

Running a terminal through `contour daemon` must not feel different from running it in-process. This
page defines what is measured, what the budgets are, and how to run the gate. For where the time goes
inside one frame, see [Profiling with Tracy](profiling.md); for the daemon itself, see
[Multiplexer & daemon mode](vthost.md).

## The budgets

Both are relative to in-process on the same machine and the same build, with **the daemon already
running**, and inclusive:

| Metric | Budget |
| --- | --- |
| Launch to first prompt pixel | at most **+10 ms** (median over in-process) |
| Keystroke to pixel over the local socket | at most **+1 ms** at the median |

A cold launch (no daemon running, so the client spawns one) is measured and reported as
`startup-cold`, never gated. The first spawn after an upgrade stages a copy of the install; it is
reported apart as `startup-cold-staging` and is not part of `startup-cold`.

Bulk output keeps coalescing at 20 ms (`FlushPolicy::coalesceDelay`); the gate says nothing about
throughput, but `bench-latency headless workload NAME` reports MiB/s and Deltas per second of both
modes beside it.

## What is timed

The application behind the terminal is always the *echo child* (`bench-latency echo-child`): it prints
the prompt `> `, then echoes every byte it reads, wrapping to a fresh prompt after 60 columns, and ends
on `^D`. What a frame shows of that is the **cursor column**, not the glyph, so the probes detect
the column the echo of key *k* leaves the cursor at (`vthost/latency/EchoProtocol.hpp`).

**Launch to first prompt.**

- `bench-latency headless`: `t0` is read just before `createTab()` (in-process) or just before the
  connect (daemon); `t1` is when the terminal's, or the mirrored screen's, cursor reaches column 2.
- `bench-latency e2e`: `t0` is the benchmark's `steady_clock` just before it spawns `contour`; `t1` is
  the `steady_clock` read in the `frameSwapped` handler of the first frame that drew the cursor at
  column 2, reported by the GUI through `--latency-probe`. `steady_clock` is one system-wide clock on
  all three platforms, and the benchmark refuses a report whose process start is before its spawn or
  after its first prompt ("steady clocks disagree").

**Keystroke to pixel.**

- `bench-latency headless`: `t0` is read by the probe immediately before the key is handed to
  `SessionHost::writeInput` (in-process) or `NativeClient::sendInput` (daemon); `t1` is when the
  cursor column reaches the key's expected column. Keys are typed 25 to 35 ms apart, so each finds the
  host idle.
- `bench-latency e2e`: `t0` is read on the GUI thread immediately before the key press event is
  delivered to the window; `t1` is read in the `frameSwapped` handler of the first frame whose
  `prepareFrameRhi` drew the cursor at the expected column. It includes key translation, the PTY write
  (in-process) or the Input PDU (daemon), the echo, parsing (in-process) or the Delta and mirror apply
  (daemon), and render plus swap. It excludes the OS input stack, which is identical in both modes.

Swaps are **unpaced** by default (`--latency-probe` selects swap interval 0), because a 60 Hz vsync
quantizes every sample to 16.7 ms, which hides a 1 ms difference. `vsync` on the `e2e` command turns
the pacing back on.

## Running it

`bench-latency` is built with `-DVTHOST_BUILD_BENCH_LATENCY=ON` (on in every preset but
`macos-package`; it is never installed and never packaged).

```sh
# Without a GUI: both modes behind the same SessionHost / daemon code.
bench-latency headless                       # 10 runs of 50 keys per mode
bench-latency headless runs 20 keys 100 record latency-headless.tsv
bench-latency headless workload ascii seconds 3   # throughput instead of keystrokes
bench-latency headless mixed seconds 60          # keys in one pane while another floods `ascii`
bench-latency headless runs 20 keys 100 keeper   # the daemon behind a PTY keeper, as `contour daemon` runs it
bench-latency headless tabs seconds 30           # a tab a second while a pane floods: each tab's time to its prompt

# With the real GUI: `contour terminal` against `contour client`. Needs a display.
bench-latency e2e contour PATH-TO-contour gate
bench-latency e2e contour PATH-TO-contour cold record latency-e2e.tsv
```

`e2e` writes a configuration with one profile (`bench`: the echo child as shell, 120x40, a steady
cursor), starts `contour daemon` on a socket of its own, and alternates `contour terminal` and
`contour client` launches, each with `--latency-probe`. Every client launch uses a unique `--label`, so
no registered service of yours answers for it. With `cold`, the benchmark stops its daemon itself
before each cold launch (the daemon's 10 s idle grace would otherwise make it warm).

On Linux the GUI needs a compositor that delivers frame callbacks. The CI-grade choice is a headless
weston: `scripts/run-display-tests.sh --run COMMAND...` runs a command under one (and under a private session
bus). Qt's
`offscreen` platform draws no frames, so a probe under it times out and the benchmark names the column
it waited for.

### Reading the table

```
metric          mode         count     p50 us     p90 us     p99 us     max us
startup         in-process      10     2620.1     3106.5     3384.2     3384.2
startup         daemon          10     3065.9     3651.7     3944.2     3944.2
keystroke       in-process     500       85.7      120.5      134.9      142.9
keystroke       daemon         500      134.1      202.4      243.0      297.4
gate: startup +0.4 ms (budget +10.0 ms) PASS; keystroke p50 +0.05 ms (budget +1.00 ms) PASS
```

Times are microseconds. The gate line compares the **medians**; a verdict is `INCONCLUSIVE`, not
`PASS`, with fewer than 10 startup or 200 keystroke samples per mode (the defaults give 10 and 500).
`headless` marks its gate line `(informational)`: the gate is `e2e gate`, whose exit status is 0 only
if both budgets pass.

A probe that never sees its cursor column ends the run with `probe timed out waiting for cursor
column N` and a non-zero exit; it never hangs. Typical causes are a blinking cursor, an unexposed
window, or a platform that schedules no frames.

## The gate

The gate is a **reproducible local run** on a Windows, a Linux and a macOS machine before the default
is flipped to daemon mode and before each release:

| Platform | Command |
| --- | --- |
| Linux | `cmake --build --preset clang-release --target contour bench-latency` then `scripts/run-display-tests.sh --run out/clang-release/src/vthost/bench-latency e2e contour out/clang-release/src/contour/contour gate` |
| Windows | `cmake --build --preset clangcl-release --target contour bench-latency` then `out\build\clangcl-release\bin\bench-latency.exe e2e contour out\build\clangcl-release\bin\contour.exe gate` (in the desktop session) |
| macOS | `cmake --build --preset appleclang-release --target contour bench-latency` then `out/appleclang-release/src/vthost/bench-latency e2e contour out/appleclang-release/src/contour/contour.app/Contents/MacOS/contour gate` (in the desktop session) |

CI runs the same benchmark on all three platforms in the `latency_trend` job and **records** the
numbers as a workflow artifact and a step summary. It never fails on them: shared runners are too
noisy to gate on, but a trend across commits shows a regression early.

## Results

Each row is one run of the gate command above, `gate cold`, 10 runs of 50 keys per mode (500 keystroke
samples). A `FAIL` or `INCONCLUSIVE` is recorded as such: the budgets and the sample minimums are not
tuned to fit.

| Date | Commit | OS | CPU | GPU | Startup delta | Keystroke p50 delta | Cold startup (p50) | Verdicts |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-10-24 | d9878cde (before the resync fix) | Fedora 44, Linux 7.2.9, Xephyr (X11, no compositor), clang-release | AMD Ryzen 9 9950X3D | none: Mesa software GL (llvmpipe) | -0.9 ms | +0.10 ms | 296.2 ms (first staging spawn 295.0 ms) | startup PASS, keystroke PASS |
| 2026-10-24 | 257505a7 (resync snapshots coalescing) | Fedora 44, Linux 7.2.9, Xephyr (X11, no compositor), clang-release | AMD Ryzen 9 9950X3D | none: Mesa software GL (llvmpipe) | -0.2 ms | +0.09 ms | 293.4 ms (first staging spawn 297.8 ms) | startup PASS, keystroke PASS |
| pending | | Windows (clangcl-release) | | | | | | to be run on the owner's machine |
| pending | | macOS (appleclang-release) | | | | | | to be run on the owner's machine |

The Linux run used a nested Xephyr server in place of the headless weston `run-display-tests.sh`
starts when it finds weston (not installed on that machine). It renders in software, so absolute times
(6.5 ms keystroke-to-pixel, 255 ms to the first prompt) are those of a software renderer; the deltas
between the two modes are what the gate judges.

### Bulk output after the flush change

`bench-latency headless runs 5 workload ascii seconds 2`, this tree against the same tree with the
commit that flushes an echo at once (`b7edc5ce`) reverted, both clang-release, two alternating runs:

| | MiB/s in-process | MiB/s daemon | Deltas/s |
| --- | --- | --- | --- |
| without the flush change | 317 / 324 | 313 / 316 | 47.9 / 48.0 |
| with it, first version (d9878cde) | 309 / 330 | 296 / 296 | 217.1 / 217.4 |
| with it, resync snapshots coalescing (257505a7) | 320 / 322 | 312 / 313 | 48.4 / 48.4 |

The first version flushed 4.5 times as often. The flood scrolls, so nearly every flush is a resync
snapshot (412 snapshots, 411 of them resyncs, in a 2 s run); a snapshot cleared the follow's increment
history, and the next screen update counted as the first one after an attach and flushed at once (422
such flushes beside 1505 coalesced). Only the attach snapshot now leaves the next increment to flush at
once; a resync keeps the history, and Deltas per second are back at the coalescing rate.

Callgrind of the daemon (`valgrind --tool=callgrind --trace-children=yes bench-latency headless runs 1
workload ascii seconds 2`, the `serve` process) shows the same profile with and without the flush
change: 829 M instructions against 846 M, with `streamSnapshots` at 37 % and `buildPush` at 15 % in both
and no new hot spot.

The keystroke gate was rerun with the fix; that run is the `257505a7` row of the results table above.

### The keeper

`contour daemon` runs the daemon behind a PTY keeper (see [the internals page](vthost.md#the-keeper)); the
keeper forks every shell, so creating a session costs one round trip over its channel, and the daemon
still reads and writes each PTY itself.

Measured on 2026-11-12, Fedora 44, Linux 7.2.13, AMD Ryzen 9 9950X3D (32 threads), clang-release. *Master* is
`9bcacb58` carrying the same bench code and the PTY resize fix (`vtpty: resize a PTY under the lock its close
takes`), so that both trees differ only by the branch; *keeper* is the branch with `keeper`. Runs are paired
in ABBA order (master, keeper, keeper, master, …) so that run order cancels out, each block under the build
lock. The machine was not idle: other sessions' tests and builds, and later a game, kept the 1-minute load
between 0.3 and 13 (mostly 2–7; per-run figures are logged with the raw results). *Noise* is master's
(p90 − p10) / median; *worse/better* counts the pairs, and *p* is a two-sided sign test over them.

| Measure (pairs) | Master | Keeper | Δ | Master noise | Worse / better | p |
| --- | --- | --- | --- | --- | --- | --- |
| `runs 10 keys 100`, keystroke p50 (20) | 112.5 µs | 121.6 µs | +8.1% | 18.6% | 16 / 4 | 0.012 |
| `runs 10 keys 100`, keystroke p99 (20) | 215.5 µs | 224.9 µs | +4.4% | 161% | 13 / 7 | 0.26 |
| `runs 10 keys 100`, startup p50 (20) | 4.61 ms | 4.95 ms | +0.34 ms | 21.5% | 16 / 4 | 0.012 |
| in-process control: keystroke p50 (20) | 67.1 µs | 69.8 µs | +4.2% | 23.0% | 13 / 7 | 0.26 |
| in-process control: startup p50 (20) | 4.23 ms | 4.51 ms | +6.6% | 19.8% | 14 / 6 | 0.12 |
| `mixed seconds 60 runs 1`, keystroke p50 (20) | 94.5 µs | 93.0 µs | −1.6% | 65.8% | 9 / 11 | 0.82 |
| `mixed seconds 60 runs 1`, flood MiB/s (20) | 339.1 | 336.4 | −0.8% | 4.2% | 14 / 5 | 0.064 |
| `workload ascii seconds 60`, MiB/s (20) | 343.6 | 341.6 | −0.6% | 7.0% | 14 / 6 | 0.12 |
| in-process control: ascii MiB/s (20) | 360.5 | 353.9 | −1.8% | 8.0% | 15 / 5 | 0.041 |

**The idle keystroke median is consistently worse on the branch**: +9 µs, in 16 of 20 pairs (p = 0.012). It is
not the keeper's doing. The same branch binary run plain and with `keeper`, 20 ABBA pairs, shows no keystroke
difference (p50 +4.7%, 11 / 9, p = 0.82; p99 −1.3%, 9 / 11), and callgrind of the plain daemon under 200 typed
keys counts the same work on both trees (loop thread 26.00 M against 26.02 M instructions, pump thread 2.21 M
against 2.16 M). What separates the trees is the rest of the branch: the in-process leg, which runs no daemon
and no keeper, moved +4.2% in the same pairs. Session creation does carry the keeper: startup is +0.34 ms
against master, and +0.41 ms (+8.8%) behind the keeper against the same binary run plain, inside the +10 ms
budget.

**Loop-thread terminal lock.** `Terminal::_stateMutex` is a `TracyLockable`; Tracy builds of both trees
(`CONTOUR_TRACY=ON`) ran `bench-latency headless tabs seconds 10` — a tab opened every second while the
first pane floods `ascii` — five ABBA pairs, the daemon's trace reduced to every hold of the lock on its loop
(main) thread. Median of the five runs' p99: master 0.51 µs, keeper 0.47 µs (worse in 0 of 5 pairs; one
tie); median hold 0.04 µs on both; the longest single hold 1.7 ms on both. The session's pump thread holds it
far longer (p99 ≈ 110 µs on both). In steady state the keeper takes the terminal lock nowhere; a daemon
recovered after a crash takes it once per restored session to seed its history (`HistorySeed`). A
Tracy-built keeper runs Tracy's threads, which the keeper refuses (it must be single-threaded); for these
runs only, a local, uncommitted change let it start anyway.

**Session creation under a flood.** `bench-latency headless tabs seconds 30 runs 1` opens a tab every second
while the first pane floods `ascii`, and times each from its request to its prompt; 10 ABBA pairs of release
builds. Behind the keeper the median tab takes 7.21 ms against master's 6.86 ms (+0.35 ms, slower in 9 of 10
pairs, p = 0.021) and the slowest 23.5 ms against 20.3 ms (10 / 0); the flood meanwhile runs 2.5% slower
(331.5 against 340.1 MiB/s, 10 / 0, p = 0.002). Against the same branch binary run plain (10 pairs), the
keeper costs a consistent +0.24 ms (+3.4%, 10 / 0) per tab — its spawn round trip — and nothing on the
flood (+0.2%, 7 / 3, p = 0.34): the flood's 2.5% is the branch's, not the keeper's, like the in-process
control's 1.8% above.

Callgrind of the daemon alone serving a 40 s `ascii` flood (about 68 MiB at the 1.7 MiB/s it manages under
callgrind) found the loop thread at 11.90 G instructions on master and 11.83 G behind the keeper, the
session's pump thread at 2.95 G and 2.98 G; the only symbols the keeper adds on those threads are
`KeeperChild`'s forwarding to its PTY and the heartbeat, under 0.01% of either.
