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
