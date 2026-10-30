# Build times

How long Contour takes to compile, measured the same way every time so a change can prove its gain.

## Method

```sh
cmake --build out/clang-release          # compile_commands.json and generated sources must be current
python3 scripts/measure-compile-times.py --build-dir out/clang-release --jobs 16 --out build-times-out
python3 scripts/measure-compile-times.py --aggregate-only build-times-out/traces --out build-times-out
```

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

`out/clang-release` (Clang 22, RelWithDebInfo), master at 4de2e780a, 16 jobs, 32-core machine:
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
