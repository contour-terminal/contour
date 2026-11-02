# Kitty graphics PR A: playback, compression, composition, daemon tick — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `a=a,s=2/s=3` animations run on their own, accept `o=z` payloads, implement `a=c` frame composition, and advance animations for daemon-hosted sessions, closing #2126 and #2108.

**Architecture:** Per-image playback state lives in `Screen::KittyAnimation`; `Screen` advances it through the existing `showKittyFrame()` and publishes the earliest deadline to `Terminal`, which advances in `tick()` under its state lock and reports the deadline through `nextRender()`. Compression is a streaming inflate in `vtbackend/graphics/` behind the existing quota. The daemon arms a per-session `core::net::DeadlineTimer` from the same deadline, throttled.

**Tech Stack:** C++23, Catch2, zlib (new, direct `vtbackend` dependency), `core::net::DeadlineTimer`.

**Spec:** `docs/superpowers/specs/2026-11-02-kitty-graphics-completion-design.md` (PR A items: playback, compression, `a=c`, daemon ticking, #2108 acceptance, daemon throttle).

## Global Constraints

- AGENT.md applies: C++23; range-for only; `enum class : uint8_t` over `bool` (negative case = 0); `std::expected` for fallible API; constructor injection; Doxygen on new public API; `[[nodiscard]]`; `.hpp` headers with `#pragma once`.
- Zero warnings under `-Werror`; no `NOLINT`, no `#pragma` mutes. Run `clang-format` on C++ only, never on `.qml`.
- Never edit `vendor/core-cpp`.
- Commits: `Signed-off-by: Christian Parpart <christian@parpart.family>` only; no Co-Authored-By or session trailers.
- Build `cmake --build --preset clang-asan`; test `ctest --preset=clang-asan`; threading tests `[threading]` under `clang-tsan`.
- Single-fragment-per-cell stays as is in this PR (decided: lifting it is PR B).
- Error replies keep the existing `CODE:message` form (`EINVAL`, `ENOENT`, `ENOSPC`, `ENOTSUP`).
- New dependency (zlib) is justified in the spec; add it to `vcpkg.json` and CMake only.
- The release notes entry in `metainfo.xml` (line ~372, "Self-running playback … not supported yet") is replaced, not appended to.

## Review Focus

- `s=2` with a single frame and no further `a=f`: stays on frame 1, no deadline, no CPU wake (Task 3).
- A frame with `z=0` ("unspecified") during playback: uses a documented default gap, never a zero-delay spin (Task 3).
- All frames gapless (`z<0`) with `s=3`: must not busy-loop; at least one frame must have a positive gap or playback holds (Task 3).
- Animation whose image is deleted (`d=i`/`d=I`/`d=A`) or whose placement scrolled off while running: no deadline remains, no crash (Task 3).
- `o=z` bomb: a tiny payload inflating past the quota answers `ENOSPC` without allocating the whole output (Task 2).
- `a=c` with identical source and destination frame and overlapping rectangles: `EINVAL` (Task 4).
- Daemon: two attached clients must not double the advance rate; a throttled session still ends on the correct frame (Task 5).

---

### Task 1: Action-aware decoding of `a=c` keys

**Files:**
- Modify: `src/vtbackend/graphics/KittyGraphics.hpp` (Command), `src/vtbackend/graphics/KittyGraphics.cpp` (`applyKey`, `parseCommand` post-processing switch)
- Test: `src/vtbackend/graphics/KittyGraphics_test.cpp`

**Interfaces:**
- Produces, in `Command` (all documented, zero = unset):
  `uint32_t composeSourceFrame` (`r=`), `uint32_t composeDestinationFrame` (`c=`), `uint32_t composeSourceX`, `composeSourceY` (`X=`, `Y=`), `uint32_t composeDestinationX`, `composeDestinationY` (`x=`, `y=`), size in the existing `sourceWidth`/`sourceHeight` (`w=`,`h=`), and `compositionMode` (existing field) set from `C=` under `a=c` (`C=1` → `CompositionMode::Replace`).
- Consumes: existing `Action::Compose`, `CompositionMode`.

- [ ] **Step 1: Write failing parse tests**
  Test names: `KittyGraphics.parse.compose_keys_are_frames_and_origins`, `KittyGraphics.parse.compose_C_selects_overwrite`, `KittyGraphics.parse.frame_X_and_Y_keep_their_meaning`.
  Assertions: for `"a=c,i=1,r=7,c=9,w=23,h=27,X=4,Y=8,x=1,y=3"` (the spec's own example): `composeSourceFrame==7`, `composeDestinationFrame==9`, `sourceWidth==23`, `sourceHeight==27`, `composeSourceX==4`, `composeSourceY==8`, `composeDestinationX==1`, `composeDestinationY==3`, `compositionMode==AlphaBlend`, `doNotMoveCursor==false`, and `columns==rows==0`. With `C=1` added: `compositionMode==Replace`, `doNotMoveCursor==false`. For `"a=f,i=1,X=1,Y=5,x=2,y=3"`: `compositionMode==Replace`, `frameBackground==5`, `sourceX==2`, `sourceY==3` (unchanged behavior; regression guard).

- [ ] **Step 2: Run them; expect FAIL** — `out/clang-asan/src/vtbackend/vtbackend_test "KittyGraphics.parse.*" ` fails on the missing fields.

- [ ] **Step 3: Implement** the new `Command` fields and the `Action::Compose` arm of the post-processing switch in `parseCommand`, following the existing `Frame`/`Animate` pattern (`std::exchange` out of `columns`/`rows`/`sourceX`/`sourceY`; `X`/`Y` are captured raw by `applyKey` into fields the `Frame` arm already converts to mode/background, so the `Compose` arm reads the same raw values as origins; `C` is moved from `doNotMoveCursor` into `compositionMode`). Doxygen every new field.

- [ ] **Step 4: Run parse tests; expect PASS**, plus the whole `[kitty]` tag.

- [ ] **Step 5: Commit** — `vtbackend: decode a=c's keys by action`.

---

### Task 2: `o=z` compression with a streaming size cap

**Files:**
- Create: `src/vtbackend/graphics/KittyInflate.hpp`, `src/vtbackend/graphics/KittyInflate.cpp`, `src/vtbackend/graphics/KittyInflate_test.cpp`
- Modify: `vcpkg.json` (add `zlib`), `src/vtbackend/CMakeLists.txt` (find and link ZLIB, add sources/test), `src/vtbackend/screen/Screen.cpp` (`validateKittyTransmission` ~5743-5746 and the frame path ~6216-6221: drop the `compression != None` refusals; inflate before decode)
- Test: `KittyInflate_test.cpp`, `KittyGraphics_test.cpp` (end-to-end)

**Interfaces:**
- Produces: `enum class InflateError : uint8_t { Corrupt, TooLarge };` and
  `[[nodiscard]] std::expected<std::vector<uint8_t>, InflateError> inflateZlib(std::span<uint8_t const> compressed, size_t maxOutputBytes);` in namespace `vtbackend::kitty_graphics`.
- Consumes: the decoded (post-base64) payload bytes; `_settings->kittyImageStorageQuota` as `maxOutputBytes` (minus bytes already stored, as `storedKittyBytes()` does).

- [ ] **Step 1: Write failing unit tests** (`KittyInflate_test.cpp`): `inflate.roundtrip` (compress 1 MiB of patterned bytes with zlib in the test, inflate, equal); `inflate.corrupt_stream_is_Corrupt` (flip a byte); `inflate.truncated_stream_is_Corrupt`; `inflate.output_over_cap_is_TooLarge_without_allocating_it` (1 GiB of zeros compresses to ~1 MiB; cap 16 MiB; result `TooLarge` and the test asserts peak capacity of the returned/internal buffer stayed ≤ cap + one chunk — expose nothing; assert via a tiny cap and `TooLarge`).
  End-to-end in `KittyGraphics_test.cpp`: `KittyGraphics.compression.transmit_with_o_z_displays_the_image` (RGBA 2x2 deflated, base64, `a=T,f=32,s=2,v=2,o=z`, reply `OK`, image fragments present), `…frame_with_o_z_is_composited`, `…bomb_answers_ENOSPC`, `…corrupt_answers_EINVAL`. The two existing ENOTSUP-for-compression tests (`KittyGraphics_test.cpp` ~111-120, 828-838) are rewritten to these expectations.

- [ ] **Step 2: Run; expect FAIL** (link error / missing function).

- [ ] **Step 3: Implement `inflateZlib`** with `inflateInit`/`inflate` in fixed-size chunks, aborting with `TooLarge` the moment output would exceed the cap. Wire it in where both payload paths decode base64, before the size check against `width*height*bpp`. `Corrupt` → `EINVAL:corrupt compressed payload`; `TooLarge` → `ENOSPC:image storage quota exceeded`. zlib wiring: `find_package(ZLIB REQUIRED)` and `ZLIB::ZLIB` on `vtbackend`; `zlib` in `vcpkg.json`.

- [ ] **Step 4: Run tests; expect PASS.** Also configure and build on the clang-asan preset from clean CMake to prove the dependency resolves.

- [ ] **Step 5: Commit** — `vtbackend: inflate o=z kitty payloads`.

---

### Task 3: Self-running playback (#2126)

**Files:**
- Modify: `src/vtbackend/screen/Screen.hpp/.cpp` (`KittyAnimation`, `controlKittyAnimation`, `transmitKittyFrame`, delete paths), `src/vtbackend/screen/Terminal.hpp/.cpp` (`tick`, `nextRender`, a published deadline), `src/vtbackend/screen/TerminalClocks.hpp` untouched (read `_clocks.steady`)
- Test: `src/vtbackend/graphics/KittyGraphics_test.cpp` (playback section), `src/vtbackend/screen/Terminal_*_test.cpp` for the `tick`/`nextRender` part, a `[threading]` case

**Interfaces:**
- Produces on `Screen`: 
  `[[nodiscard]] std::optional<std::chrono::steady_clock::time_point> advanceKittyAnimations(std::chrono::steady_clock::time_point now);` — advances every running animation whose deadline ≤ `now` through `showKittyFrame()` (as many frames as are due, catching up at most one full loop), returns the earliest remaining deadline or nullopt when nothing runs. Caller holds the state lock.
  `KittyAnimation` gains `std::chrono::steady_clock::time_point nextFrameDue`, `uint32_t loopsDone`, and `AnimationState state` is the existing field.
- Produces on `Terminal`: the deadline is stored in `std::atomic<std::chrono::steady_clock::time_point> _kittyAnimationDue` (max = none), `tick()` calls `advanceKittyAnimations(_clocks.steady.now())` under `_ownStateLock` only when `_kittyAnimationDue` is not max (same discipline as `_isGliding`), `nextRender()` folds `max(due - steadyNow, 0)` into `nextWakeup`, and the change is announced via `Events::renderBufferUpdated()` when playback starts so an idle display re-queries `nextRender()`.
- A named constant `constexpr std::chrono::milliseconds DefaultKittyFrameGap { 40 };` documents the gap used for a frame whose `z=` is 0 ("unspecified").
- Consumes: `showKittyFrame(shared_ptr<Image const> from, shared_ptr<Image const> to)`, `Terminal::steadyNow` via `_clocks.steady`.

Semantics (from #2126 and the spec; each is a test below):
`s=3`: frame 1 → … → last → frame 1, forever when `v=1` or `v=0`; `v=n` (n>1) stops after n−1 loops on the last frame. `s=2`: same, but at the last frame the animation holds with no deadline until `a=f` adds a frame, then continues from it. `s=1` stops on the current frame and drops the deadline. A frame with `z<0` is skipped. `z=0` uses `DefaultKittyFrameGap`. If every frame is gapless, playback holds on frame 1 (no deadline). `a=a,c=N` still selects a frame immediately in every state. `s=2`/`s=3` now answer `OK`. A deleted image drops its animation and deadline.

- [ ] **Step 1: Write failing tests** driving `MockTerm`'s `ManualClock steadyClock`: 
  `KittyGraphics.playback.s3_advances_frames_by_their_gaps` (3 frames gaps 10/20/30 ms; advance clock 10 ms → `tick` → frame 2 shown; etc.; asserts the fragment's image is frame N's image each step; reply `OK`), `…s3_wraps_to_frame_1`, `…v_n_stops_after_n_minus_1_loops`, `…s2_holds_at_last_frame_then_continues_on_new_frame`, `…s1_stops_on_current_frame`, `…gapless_frames_are_skipped`, `…zero_gap_uses_the_default`, `…all_gapless_holds_without_a_deadline`, `…explicit_frame_selection_works_while_playing`, `…deleted_image_drops_deadline` (`nextRender()==nullopt`), `…scrolled_off_placement_holds_no_deadline`, `…nextRender_reports_time_to_next_frame` (`nextRender()==10ms` after start, `0ms` once due). A `[threading]` case: a thread calls `tick()` in a loop while the main thread feeds `a=f`/`a=a` bytes; no data race under TSan.

- [ ] **Step 2: Run; expect FAIL** (`ENOTSUP` still returned, no `advanceKittyAnimations`).

- [ ] **Step 3: Implement** the interface above. In `controlKittyAnimation` replace the `ENOTSUP` branch: set state/loop count, compute `nextFrameDue` from the injected steady clock, publish the earliest deadline to `Terminal`. In `transmitKittyFrame`, if the animation is in `RunAwaitingFrames` and holding at its last frame, resume. "No placement on the page" means `updateKittyFragments` replaced zero fragments for the image; then the animation holds no deadline until a placement appears (a later `a=p`/`a=T` re-arms it). `tick()` drops `noexcept` or wraps the advance in a `try`/`catch (std::bad_alloc const&)` that stops the animation — pick one and document it. Fire `Events::renderBufferUpdated()` after an advance and when a deadline appears or vanishes.

- [ ] **Step 4: Run all `[kitty]` tests and the `[threading]` case under `clang-tsan`; expect PASS.**

- [ ] **Step 5: Commit** — `vtbackend: play kitty animations on their own clock`.

---

### Task 4: Frame composition (`a=c`)

**Files:**
- Modify: `src/vtbackend/screen/Screen.hpp/.cpp` (`composeKittyFrames`, the `Action::Compose` case that currently replies `ENOTSUP:frame composition not supported`, ~5980); reuse `compositeKittyRectangle` (Screen.cpp ~6158)
- Test: `KittyGraphics_test.cpp`

**Interfaces:**
- Consumes (Task 1): `Command::composeSourceFrame`, `composeDestinationFrame`, `composeSourceX/Y`, `composeDestinationX/Y`, `sourceWidth/Height`, `compositionMode`.
- Produces: `void Screen::composeKittyFrames(kitty_graphics::Command const& command);`

Semantics (kitty spec): copy the `w×h` rectangle at source origin of frame `r` onto the destination origin of frame `c` of image `i`; `w`/`h` default to the whole image; alpha-blend unless `C=1`. `ENOENT` for a missing image or frame; `EINVAL` when either rectangle leaves the image, or when source and destination frame are the same and the rectangles overlap; `ENOSPC` when the new frame copy would exceed the quota. The destination becomes a copy-on-write new `Image` (frames are immutable shared images); if it is the current frame, refresh placements via `showKittyFrame()`.

- [ ] **Step 1: Write failing tests**: `KittyGraphics.compose.alpha_blends_a_rectangle` (assert exact blended pixel values for a half-transparent source over opaque red), `…C1_overwrites`, `…defaults_to_the_whole_image`, `…missing_image_or_frame_is_ENOENT`, `…out_of_bounds_is_EINVAL`, `…same_frame_overlap_is_EINVAL`, `…same_frame_disjoint_is_allowed`, `…over_quota_is_ENOSPC`, `…current_frame_refreshes_the_placement`, and a cross-check test that the blend arithmetic equals what `a=f` uses for the same inputs.
- [ ] **Step 2: Run; expect FAIL** (`ENOTSUP`).
- [ ] **Step 3: Implement** by sharing `compositeKittyRectangle` with the `a=f` path (no second blend implementation). Remove the `ENOTSUP` reply.
- [ ] **Step 4: Run `[kitty]` tests; expect PASS.** Manually cross-check three of the blend cases against kitty 0.49.2 (installed) and record the result in the test comments.
- [ ] **Step 5: Commit** — `vtbackend: compose kitty animation frames (a=c)`.

---

### Task 5: Daemon ticking with a throttle

**Files:**
- Modify: `src/vthost/SessionHost.hpp/.cpp` (a per-session `std::optional<core::net::DeadlineTimer> _animationTick`, armed from the terminal's deadline; modelled on `_foregroundPoll`, SessionHost.cpp:1725-1739), `src/vthost/NativeSession.cpp` only if Delta emission needs the existing `screenUpdated` path
- Test: `src/vthost/SessionHost_test.cpp` (or the nearest existing suite carrying the `daemon` label)

**Interfaces:**
- Consumes: `Terminal::nextRender()` (deadline in ms), `Terminal::tick(time_point)`, `Events::screenUpdated()` (existing path to Delta emission), the loop's clock `_loop.clock().now()`.
- Produces: `constexpr std::chrono::milliseconds DaemonMinimumFrameInterval { 100 };` in `SessionHost.hpp` (builder may tune; the test uses the constant, not the literal). Behavior: when `renderBufferUpdated()` reports a changed wakeup, the loop thread reads `nextRender()`, arms the timer for `max(that, remaining throttle)`, and on fire takes the terminal lock, calls `tick(now)`, fires `screenUpdated`. Frames due faster than the interval are collapsed: `advanceKittyAnimations` is called with the latest `now`, so it lands on the correct frame by time, not by count.

- [ ] **Step 1: Write failing tests** with the loop's injectable clock: `daemon.playback_advances_frames_for_a_hosted_session` (3-frame `s=3` animation, advance loop clock, assert the session's screen shows frame 2, and a Delta was emitted), `daemon.playback_is_throttled_to_the_minimum_interval` (10 ms gaps, advance 1 s: emitted Deltas ≤ `1s / DaemonMinimumFrameInterval + 1`, and the final frame equals the by-time frame), `daemon.two_clients_do_not_double_the_rate`, `daemon.idle_session_arms_no_timer`.
- [ ] **Step 2: Run `ctest -L daemon`; expect FAIL.**
- [ ] **Step 3: Implement** the timer re-arm on `renderBufferUpdated()` (hand the work to the loop with `EventLoop::post`, per that event's documented rules: no terminal-state reads from the callback) and the throttle.
- [ ] **Step 4: Run `ctest -L daemon` and the `[threading]` tests under `clang-tsan`; expect PASS.**
- [ ] **Step 5: Commit** — `vthost: tick kitty animations for hosted sessions`.

---

### Task 6: #2108 acceptance test, release notes, final checks

**Files:**
- Test: `KittyGraphics_test.cpp` (byte-count test)
- Modify: `metainfo.xml` (~line 372)

- [ ] **Step 1: Write the byte-count test** `KittyGraphics.frames.a_delta_frame_transmits_only_its_rectangle`: send a 64×64 RGBA image, then an `a=f,x=…,y=…,s=8,v=8` delta; assert the delta's payload bytes (base64-decoded) equal `8*8*4`, the resulting frame equals base + patch, and the total bytes the terminal accepted for N delta frames is `< (N * 64*64*4) / 20` for N=30 (the saving #2108 reports, here as a lower bound the test pins).
- [ ] **Step 2: Run; expect PASS** (the delta path exists from #2109; this pins it).
- [ ] **Step 3: Update `metainfo.xml`**: replace the sentence "Self-running playback (`s=2`/`s=3`) and frame composition (`a=c`) are not supported yet (#2108)" with an entry for self-running playback, `o=z` payloads, `a=c`, and daemon playback (#2108, #2126).
- [ ] **Step 4: Full verification**: `cmake --build --preset clang-asan` (zero warnings), `ctest --preset=clang-asan`, `ctest -L daemon`, `[threading]` under `clang-tsan`; manual check of `kitten icat <animated.gif>` in a built Contour (generate one with `magick`/`ffmpeg`); coverage run on the changed files. Report performance impact (idle terminal: no wakeups with no animation), risk, and coverage in the PR body.
- [ ] **Step 5: `/simplify`, then `/code-review`** on the branch; address every finding.
- [ ] **Step 6: Commit** — `docs: release notes for kitty animation playback`; open the PR with `Closes #2126` and `Closes #2108`.
