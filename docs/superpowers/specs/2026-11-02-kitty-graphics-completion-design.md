# Kitty graphics: completing the protocol

Date: 2026-11-02
Closes: #2126, #2108

## Intent

Contour implements most of the kitty graphics protocol and refuses the rest with `ENOTSUP`.
Refusing is honest, but it pushes clients onto expensive fallbacks (#2108 measured 15x the bytes
for notcurses video) and leaves animated GIFs frozen on frame 1 (#2126). This work removes the
refusals one by one, so that a client written against the kitty spec works in Contour without
feature probing.

## Scope

Four pull requests, stacked in this order, one commit per feature inside each. (Decided with the
maintainer after the second review: one PR was not realistic for this much surface. The first
version of this document said one PR.)

- **PR A** — playback, `o=z` compression, `a=c` composition, daemon ticking.
  Closes #2126 and #2108.
- **PR B** — placement table, fragment model, image numbers (`I=`), remaining delete targets.
- **PR C** — local media (`t=f`/`t=t`/`t=s`) and the consent policy.
- **PR D** — Unicode placeholders and relative placements.

The seven features, with their PR:

1. **Self-running playback** (#2126, A): `a=a` with `s=2` / `s=3`.
2. **Compression** (A): `o=z` (zlib) for transmissions and for frames.
3. **Local media** (C): `t=f` (file), `t=t` (temp file), `t=s` (POSIX shared memory).
4. **Remaining delete targets** (B): `d=n p c q r x y z` and their upper-case forms.
5. **Frame composition** (A): `a=c`, with `C=1` selecting overwrite instead of alpha blend.
6. **Unicode placeholders** (D): `U=1`, the U+10EEEE placeholder cells.
7. **Relative placements** (D): `P=`, `Q=`, `H=`, `V=`.

### Open decisions (not yet made by the maintainer)

Each blocks the PR named. Until decided, the builder must not pick a side.

- **Blocks A — #2108 acceptance.** The "notcurses must show the frame-delta path" criterion is not
  attainable here: notcurses picks its animated path by detecting kitty's version, so re-running the
  demo gives the same byte count. Recommended: replace it with a byte-counting test in
  `vtbackend_test` that drives `a=f` deltas, plus an upstream notcurses request. Closing #2108 on
  that basis needs the maintainer's yes.
- **Blocks A — daemon playback cost.** Every advanced frame is a full image on the wire (~25 MB/s
  per client for a 1 MB GIF at 40 ms). Recommended: throttle on the daemon path. Alternative: send
  the first frame only and defer client-side animation.
- **Blocks B — fragment model.** One image fragment per cell means a relative child overlapping its
  parent destroys the parent's cells, and `d=z`/`d=q` can only remove what is on top. Recommended:
  lift the limit first (per-cell vector ordered by z; wire carries N). Alternative: keep it and
  restrict relative placements to non-overlapping children.
- **Blocks C — file access seam.** `core::platform::FileSystem` lives in `vendor/core-cpp` and cannot
  be edited here, and lacks descriptor access, temp-dir lookup and `shm_open`. Recommended: a small
  vtbackend-local media-source seam (native implementation plus an in-memory fake).
- **Blocks C — parking rules.** Stale resolution (page scrolled, screen switched, reset while
  parked): recommended answer `EPERM` and place nothing. `a=q,t=f`: recommended to answer from the
  policy without a dialog. Late replies: recommended an auto-deny deadline on the ask.

### Non-goals

- Windows shared memory (`t=s` answers `ENOTSUP` there, with a clear message).
- Anything outside the kitty graphics protocol specification.

### Anti-goals (failure even if it technically works)

- Answering OK for something that is not actually done. A client must never believe an animation
  runs, or a placement is gone, while the screen says otherwise. The existing honest-`ENOTSUP`
  discipline stays for whatever remains unsupported.
- An idle terminal burning CPU because an animation's placement is off screen.
- A file-reading primitive reachable by an unprivileged program (see Local media).

## Constraints

- AGENT.md applies in full: C++23, `std::expected`, `enum class` over `bool`, constructor
  injection, zero warnings under `-Werror`, no `NOLINT`, Doxygen on new public API.
- Layering: `vtbackend/graphics/` produces images, `vtbackend/screen/` owns the engine. Lower layers
  must not include higher ones. Clock and file access are reached through injected interfaces.
- Every feature lands with Catch2 tests; threading-relevant parts carry `[threading]` and run
  under `clang-tsan`.
- `vendor/core-cpp` is not edited.

## Design

### 1. Self-running playback (#2126)

- A clock interface is injected into the component that owns animations. Tests step time
  explicitly; production uses the existing platform clock.
- **Next-deadline scheduling.** The earliest frame deadline across running animations is tracked
  and the terminal is woken at it. No polling.
- **Advance** goes through the existing `Screen::showKittyFrame()`, which finds placements in the
  grid, so scrolling, erasing and reflow stay correct.
- **Protocol:** `s=1` stops on the current frame. `s=2` plays and waits at the last frame until
  another `a=f` arrives, then continues. `s=3` plays and returns to frame 1 after the last frame.
  `v=`: 0 unspecified, 1 forever, n means n-1 loops. `z<0` is a gapless frame and is skipped.
  `a=a,c=N` (application-selected frame) keeps working while playing or stopped.
- **Pausing** (builder's call, per the issue's "probably"): an animation with no placement on the
  page holds no deadline. "Terminal not visible" is defined as "no `tick()` arrives": `Terminal` has
  no visibility flag, and the display stops calling `tick()` when it does not paint, so this pauses
  for free. Only the active screen's animations hold a deadline (`_kittyImages` is per `Screen`).
  `tick()` is `noexcept` today but advancing allocates, so the advance either catches `bad_alloc`
  or `tick()` drops `noexcept`; a tick that advances must also fire `screenUpdated()`.
  A virtual (`U=1`) placement has no fragments for `showKittyFrame()` to find, so PR D resolves the
  image's current frame at render-buffer build time instead.
- **Where it lives (builder's call, from review):** the deadline is checked in `Terminal::tick()`
  under the terminal's own state lock, as the glide animation already is, and published through
  `nextRender()`, so the display wakes at it. Time comes from `TerminalClocks::steady` (which
  `MockTerm` already drives with a `ManualClock`), not from the push-based `currentTime()`, which
  would drift. Frames are grid state under the state mutex; the render buffer is not the precedent.
- **Daemon mode (decided with the maintainer: in scope).** `vthost` does not call
  `tick()`/`nextRender()` today, so playback would not advance for daemon-hosted sessions. The PR
  wires the daemon's session host to tick, and `ctest -L daemon` covers playback.
- **Threading:** `[threading]` tests exercise tick against concurrent parser-thread commands and run
  under `clang-tsan`.
- Replaces the `ENOTSUP:self-running animation is not supported` reply.

### 2. Compression (`o=z`)

Inflate zlib payloads before they reach the existing decode path, for `a=t`/`a=T`/`a=q` and `a=f`.
The inflated size is checked against the existing image quota *while inflating* (streaming), so a
small payload cannot expand without bound. A corrupt stream answers `EINVAL`.

**Decided with the maintainer: zlib becomes a direct dependency of `vtbackend`.** It was not one
before (not in `vcpkg.json`, CMake or `cmake/`). Justification under AGENT.md's dependency rule: the
streaming size cap, `o=z` working in the daemon, real inflation in `vtbackend_test`, and #2108's
byte measurement all need it. It is a single small library available on all three platforms; the
builder adds it to `vcpkg.json` and the CMake wiring for each platform. The PNG path, which is
injected from the GUI via `QImage`, is unchanged.

### 3. Local media

Three layers, decided with the maintainer:

1. **Hard rules, no knob.** Only regular files are read; devices, FIFOs, directories and anything
   under `/proc`, `/sys`, `/dev` are refused. The check runs on the opened descriptor. The existing
   image quota caps size. `t=t` is accepted only for a file in the system temp directory whose name
   contains `tty-graphics-protocol`, and the terminal deletes it after reading. `t=s` is read once
   and unlinked. Offsets and sizes (`O=`, `S=`) are honoured as the spec defines them.
2. **Consent policy for `t=f` only**, as `Deny | Ask | Allow`, default `Ask`. `Ask` goes through
   the guarded-role consent flow `TerminalSession` already has (allow, optionally remember), so the
   user sees the path first. This matters over SSH, where the path names a file on the machine
   running the terminal. `t=t` and `t=s` default to allowed: their naming rule means the program
   had to create the file itself. (Builder's call: whether the `t=t`/`t=s` policy gets its own
   setting; default is no.)
3. **File access behind an injected interface**, so tests use an in-memory fake and never touch the
   real filesystem.

Config follows the existing config-enum pattern (`PermissionsConfig`); `vtbackend` gets its own
Qt-free policy enum, because `vtbackend` must not depend on the config layer above it (the config
enum is `config::Permission`). `vtbackend` itself
never prompts: it asks the injected policy, and the GUI layer implements `Ask`.

**Ask is asynchronous, the protocol is not (decided with the maintainer: park the command).** The
guarded-role consent flow (`TerminalSession::requestCaptureBuffer`) is raised on the parser thread,
posted to the GUI thread, and answered later, while the parser thread moves on. So a `t=f`
command under `Ask` is *parked*: queued together with the cursor position it was received at, and
executed and answered when the user decides (`a=T` then places at that remembered position, not the
cursor's current one). Commands that follow in the stream and depend on it (an `a=p,i=N` for the
parked image) must still see the right result: the parked command blocks later kitty commands for
the same image id until resolved, so they queue behind it rather than answering `ENOENT`. `a=q,t=f`
is a query and answers through the same path. The parked queue is bounded; overflow answers
`EPERM` rather than growing.

Wherever there is no prompt surface (headless builds, `vthost::SessionHost`), `Ask` resolves to
`Deny` *with a reply* (`EPERM`). "Remember allow" is per role, as for the other guarded roles, so it
covers later paths in that session; that matches existing behaviour and is intended.

Refinements (builder's calls, from review): `/dev/shm` is accepted for `t=t` as kitty does, while the
rest of `/dev`, `/proc`, `/sys` are refused. File access extends the existing
`core::platform::FileSystem` seam that `Terminal` already injects (descriptor-based open and
`shm_open`/unlink are added to it) rather than introducing a parallel interface.

### 4. Remaining delete targets

Add, per the spec: `n`/`N` (by image number, from `I=`), `c`/`C` (placements intersecting the
cursor), `p`/`P` (placements intersecting the cell named by `x=`,`y=`), `q`/`Q` (that cell plus
`z=`), `r`/`R` (image id range from `x=`..`y=`), `x`/`X` (column), `y`/`Y` (row), `z`/`Z`
(z-index). As with `a=c`, `x`/`y` mean something different here than under other actions, so the
parser needs action-aware decoding for `a=d` too. Upper-case
also frees image data once no placement refers to it, matching the existing `a`/`A`, `i`/`I`
behaviour. Implemented as a table from target letter to a placement predicate, so a further target
is one row (data-driven design). The default-case `ENOTSUP` goes away.

### 5. Frame composition (`a=c`)

Compose a rectangle from a source frame onto a destination frame within one image. Keys, checked
against the kitty spec: `r=` source frame, `c=` destination frame (both 1-based), `X=`/`Y=` source
origin, `x=`/`y=` destination origin, `w=`/`h=` size (default: the whole image), and `C=1` for plain
overwrite (default: alpha blend). Note the key reuse: `C`, `X`, `Y`, `x`, `y`, `c`, `r` mean
something else under other actions, and today the parser maps `C`→doNotMoveCursor, `X`→compositionMode
and `Y`→frameBackground unconditionally, so `a=c` needs action-aware decoding of these keys.
Errors per spec: `ENOENT` for a missing image or frame, `EINVAL` for rectangles out of bounds or for
overlapping rectangles when source and destination frame are the same, `ENOSPC` when the quota is
exceeded. Where the spec leaves an edge case to the implementation, behaviour is cross-checked
against kitty 0.49.2 (installed locally) and recorded in the tests. After composition, if the
destination is the current frame, the placement is refreshed through the same path as playback.

### 6. Unicode placeholders (`U=1`)

A placement created with `U=1` draws no image directly. Instead the image appears wherever the
application prints the placeholder character U+10EEEE:

- the image id comes from the cell's foreground colour (256-colour and 24-bit forms; the third
  most-significant byte from an optional third diacritic),
- the placement id from the underline colour,
- row and column from the diacritic that follows the placeholder, using the spec's diacritic table,
  with omitted ones inferred from the previous cell as the spec describes.

The decision "which image slice does this cell show" is a pure function from (cell, previous cell,
placement geometry) to (image id, placement id, row, column) and is extracted into a
dependency-free header so it is table-testable. The rasterizer consumes the result through the same
path as ordinary placements. This is what lets tmux and other multiplexers carry images.

### 7. Relative placements

`P=` (parent image id), `Q=` (parent placement id), `H=`/`V=` (cell offsets). A child placement is
positioned relative to its parent and follows it. Deleting the parent deletes its children. Errors
per spec: a missing parent answers `ENOPARENT`, a reference cycle `ECYCLE`, a chain deeper than the
implementation's maximum `ETOODEEP` (the spec requires supporting at least 8 levels), and making a
*virtual* (`U=1`) placement relative `EINVAL`. Positions are recomputed from the parent, never
cached in absolute form, so scrolling and reflow keep working. Depends on the placement table
described under Cross-cutting.

## Cross-cutting prerequisites (decided with the maintainer)

Review found that sections 4, 6 and 7 stand on two things the code does not have. Both are in scope
and come before the features that need them.

**Placement table.** Today a placement is only per-cell image fragments (`Line::imageFragments`);
`showKittyFrame()` deliberately keeps no placement record. Unicode placeholders (`U=1`) draw no
cells, so virtual placements need a registry; `Q=`, `d=r` and `d=i,p=N` need placement ids; `d=z` and
`d=q` need the integer z-index, which is collapsed to `ImageLayer::Below/Above` today. So: a
placement table keyed by (image id, placement id) is added **beside** the grid walk. The grid stays
the source of truth for what is drawn; the table carries identity, z, parent and virtual-ness.
Fragments gain the placement id and z they were made from. Open for the builder: the
single-fragment-per-cell limit means two placements with different z cannot overlap in one cell;
keep that limit and document it unless the fragment model change turns out small.

**Image numbers (`I=`).** Today `imageNumber` is only echoed in replies. `d=n` needs it for real:
`a=t,I=N` makes the terminal assign an image id and remember number→id, the latest transmission
winning for a given number, and the reply carries the assigned id as the spec requires.

## Acceptance

- Each feature's `ENOTSUP` reply is gone and its behaviour matches the kitty spec, verified by
  tests; whatever is still refused (Windows `t=s`) is documented as such.
- #2126's acceptance list holds: `a=a,s=3` answers OK and advances by gaps; `s=2` waits and
  continues; `v=n` stops after n-1 loops; `s=1` stops; `c=` still works during playback; tests use
  an injected clock for order, gaps, loops, gapless frames, the `s=2` wait, and a scrolled-off or
  deleted placement.
- `kitten icat` of an animated GIF animates in Contour (manual check, kitten permitting).
- #2108 closes with a measurement: the issue's `notcurses-demo -c -d 0 x` byte accounting is re-run
  and the numbers go in the PR. It must show the frame-delta path in use; the exact ratio is
  reported, not promised.
- Zero warnings; `ctest --preset=clang-asan` and the `[threading]` tests under `clang-tsan` pass;
  coverage of the new code is reported; the release notes in `metainfo.xml` are updated, replacing
  the line that says self-running playback and composition are unsupported.
- Final `/code-review` with all findings addressed.

## Left to the builder

Internal structure (where the scheduler lives, how the placeholder cell decision is split), test
organisation, exact config key names, and commit ordering. Suggested order: playback (including
daemon ticking), compression (zlib dependency), placement table and image numbers, delete targets,
composition, local media (including parking), placeholders, relative placements.
