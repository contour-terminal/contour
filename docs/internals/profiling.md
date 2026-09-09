# Profiling with Tracy

Contour is instrumented for the [Tracy](https://github.com/wolfpld/tracy) frame profiler, so the
whole pipeline — PTY read → VT parse → grid write → render buffer → frame — can be measured rather
than guessed at, and two commits can be compared mechanically.

Call sites use Tracy's own macros directly. There is deliberately no first-party profiling
abstraction: `ZoneScoped` and `TracyPlot` mean exactly what Tracy's documentation says they mean.

## Options

All default to `OFF`, and with all of them off nothing is fetched and no compiled output changes —
`contour::tracy` merely puts a no-op header on the include path.

| Option | Builds | Cost |
| --- | --- | --- |
| `CONTOUR_TRACY` | Instrumentation compiled into contour (the client). | Fetches `TracyClient` (a single source file). |
| `CONTOUR_TRACY_TOOLS` | `tracy-capture`, `tracy-capture-daemon`, `tracy-csvexport`. | Tracy CPM-fetches capstone, zstd, PPQSort and json. Minutes, once. |
| `CONTOUR_TRACY_GUI` | The `tracy-profiler` GUI. | Adds glfw, freetype, imgui, nativefiledialog, md4c, base64, tidy, usearch, pugixml and libcurl, and needs EGL/Wayland (or X11) development packages. |
| `CONTOUR_TRACY_GUI_X11` | Selects X11 over Wayland for the GUI. | — |

The tool options are independent of `CONTOUR_TRACY`: build the viewer and the exporter once and keep
them, and rebuild only contour when you want a different instrumented binary. Neither tool is
packaged on Arch, which is why they are buildable from this tree at all.

`TRACY_VERSION` in `cmake/Tracy.cmake` pins all three. The client and the tools must be the same
version — Tracy's capture protocol is version-specific.

## Building a profiling tree

```bash
cmake --preset clang-release -B out/clang-tracy \
    -DCONTOUR_TRACY=ON -DCONTOUR_TRACY_TOOLS=ON
cmake --build out/clang-tracy -j4
```

**Do not profile the default preset.** `clang-asan` is Debug plus AddressSanitizer and
UndefinedBehaviorSanitizer; its timings say nothing about release behaviour. `clang-release` is
`RelWithDebInfo`, which keeps the symbols Tracy needs.

The tools are not part of `all` — build them by name:

```bash
cmake --build out/clang-tracy -j4 --target tracy-tools   # capture + csvexport
cmake --build out/clang-tracy -j4 --target tracy-gui     # the profiler GUI
```

Both land in `out/clang-tracy/tracy-tools/bin/`. If the GUI's configure step fails with
`No package 'egl' found` (or `wayland-egl`, `wayland-cursor`, `xkbcommon`), install those development
packages or reconfigure with `-DCONTOUR_TRACY_GUI_X11=ON`.

## Taking a capture

**Live, with the GUI.** Start `tracy-profiler`, run the instrumented `contour`, and connect. The
frame view shows the terminal's real frame rate, because there is exactly one `FrameMark` in the
tree.

**Scripted, to a file.** `tracy-capture` waits for a client, so start it first. Give it `-s` so the
capture ends on its own rather than waiting for the client to disconnect:

```bash
out/clang-tracy/tracy-tools/bin/tracy-capture -o /tmp/notcurses.tracy -f -s 50
```

then, in another terminal, a workload — `notcurses-demo` is a good one, since `-c` pins its PRNG
seed and `-d 0` removes its pacing delays, making the run repeatable and parse/render-bound:

```bash
out/clang-tracy/src/contour/contour early-exit-threshold 0 \
    execute sh -c 'notcurses-demo -c -d 0 -p /usr/share/notcurses ixetun'
```

The client listens on `127.0.0.1:8086` from process start, so the two can be started in either
order. `TRACY_NO_EXIT=1` in the environment makes the client hold its data until a profiler has
collected it — use it for a run too short to attach to, but not together with `-s`, or the two wait
on each other. The same recipe profiles `bench-headless`, which inherits every backend zone by
linking `vtbackend`.

## Reading a capture

```bash
out/clang-tracy/tracy-tools/bin/tracy-csvexport /tmp/contour.tracy
```

One row per zone: name, source location, total time, share of the whole, call count, and
mean/min/max/stddev. This is the terminal-readable form — no GUI needed — and it is how two commits
are compared: capture the same workload on each, export both, and diff the totals for the zones you
changed.

Two things to know when reading it:

- **`readFromPty` will dominate every capture.** It is the blocking read, so its total is mostly
  idle. That is the point of zoning it — the wait is accounted for rather than sitting in an
  unexplained gap — but it means the interesting number is everything *below* it.
- **A zone in a template appears once per instantiation**, with the same name and source line. Both
  `Parser::parseFragment` rows come from `Parser-impl.hpp`; they are different instantiations of the
  parser, not a double count.
- A zone that was never entered is simply absent from the export.

## Zone and plot inventory

### Parser thread (`Terminal.Loop`)

| Zone | Where |
| --- | --- |
| `Terminal::processInputOnce` | `vtbackend/screen/Terminal.cpp` |
| `Terminal::readFromPty` | The blocking read — what separates idle from busy. |
| `Terminal::writeToScreen` | Locally injected text only; PTY input does not pass through it. |
| `Terminal::parseFragmentChunked` | Same — the injection path, not the PTY path. |
| `Parser::parseFragment` | `vtparser/Parser-impl.hpp` — the state machine. |
| `Screen::writeText` | The **bulk** overload; carries its cell count as the zone value. |
| `Screen::writeTextEnd` | |
| `Screen::executeControlCode` | One per control byte (LF, CR, BEL …). |
| `Screen::processSequence` | One per escape sequence, **named after the sequence** (see below). |
| `Screen::processAPC` | |
| `Screen::scrollUp` / `scrollDown` / `clearScreen` | |
| `Terminal::screenUpdated` | The parser thread's hand-off to the GUI. |
| `Terminal::resizeScreen` | |
| `Screen::applyPageSizeToMainDisplay` | The main grid's reflow. |
| `Terminal::fillRenderBufferInternal` | |
| `Grid::resize` | `vtbackend/grid/Grid.cpp` — reflow. Three call sites aggregate here: the main display and the two one-line status grids. `applyPageSizeToMainDisplay` is what separates them. |

`Screen::processSequence` renames its zone to the sequence's mnemonic, so the timeline reads `SGR`,
`CUP`, `ED` rather than a wall of identical rows. Two caveats: the rename is visible **only in the
GUI** — `tracy-csvexport` aggregates by source location, so the CSV keeps one `processSequence` row —
and it costs about 12 ns per sequence, which is measurable against a ~20 ns dispatch but negligible
in absolute terms (~4 ms across 324k sequences).

`Screen::writeText(char32_t)` and `writeTextInternal` are deliberately **not** instrumented: they run
once per codepoint, so a zone there would cost more than the work it measures and would bury the
trace. The bulk `writeText(string_view, size_t)` overload above is the instrumented one.

Plots: `pty.read.bytes` (bytes returned by each PTY read) and `pty.parse.bytes` (bytes handed to the
parser). Together they show throughput and how the stream is chunked.

### GUI / render thread

| Zone | Where |
| --- | --- |
| `TerminalDisplay::prepareFrameRhi` | Pipeline build and resource uploads (before `beginPass`). |
| `TerminalDisplay::paint` | Called from `prepareFrameRhi`; drives `Renderer::render`. |
| `TerminalDisplay::recordFrameRhi` | Draw-command recording. Ends with the frame's `FrameMark`. |
| `Renderer::renderImpl` | Opens before `_applyMutex` is taken, so the wait is inside the zone. |
| `Renderer::renderCells` / `renderLines` / `renderGutter` | |
| `TextRenderer::beginFrame` / `endFrame` | |
| `OpenShaper::shape` / `rasterize` | Shaping and rasterization — where a cache miss becomes a hitch. |

### Lock contention

`Terminal::_stateMutex` is declared through `TracyLockable`, so Tracy's lock view shows how long the
render path waits on the parser thread and vice versa. That is the cross-thread question the
per-thread zones cannot answer on their own.

## Adding instrumentation

Use Tracy's macros directly — `#include <tracy/Tracy.hpp>` and `ZoneScoped;`.

Any macro you use must also be defined in `src/crispy/tracy-stub/tracy/Tracy.hpp`, the no-op header
that stands in when `CONTOUR_TRACY=OFF`. `ctest -R check_tracy_stub` enforces this, and it matters:
a macro missing from the stub breaks the *default* build — the one CI and every packaging job run —
while the profiling build stays green.

The stub is a faithful proxy, not merely a permissive one: it declares the same
`___tracy_scoped_zone` name the real macros declare, so two zone macros in one scope collide and
`ZoneText`/`ZoneValue` without a zone in scope fail, exactly as they do with Tracy enabled. It is
`constexpr` and odr-used only inside `sizeof`, so no storage is emitted and disabled macros never
evaluate their arguments.

That fidelity is not complete, though — the stub models the macros, not the library. Build with
`-DCONTOUR_TRACY=ON` before pushing instrumentation changes.
