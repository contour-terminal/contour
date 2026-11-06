# Images in daemon-hosted panes: lay out on the client's real cell size

Status: design, awaiting approval. Supersedes PR #2212.

## Why

A Claude session spawned a pane through the daemon and ran a TUI demo that printed a sixel. The
daemon died with SIGFPE and took every hosted shell with it. The cause: a daemon's
`vtbackend::Terminal` is never told a cell size in pixels — clients report rows and columns only
(`ResizeRequest`, `ResizePane`), and `SessionHost` resizes by cells — so `cellPixelSize()` stays
0×0 and image layout divided by it.

Master no longer crashes: #2208 (`9cc2c0cc2`) drops an image while the cell size is unknown, and
guards the VT340 cursor-quirk modulo. But that only trades the crash for an image that never shows.
PR #2212 then proposed laying every image out on a fixed 10×20 cell instead. That is not acceptable
either: the daemon decides how many cells an image covers, and every client then stretches the
image into that many of its own cells (`ScreenMirror.cpp`, `ResizeToFit` for sixel), so no sixel in
a daemon pane would ever show at its real pixel size.

The cause to fix is that the daemon does not know the cell size. This design makes clients tell
it.

## Goals

- Sixel, GIP (Contour's own protocol), kitty and every other image path in a daemon-hosted pane is
  laid out on the cell size of a real client, so it shows at its true pixel size on that client.
- A client that was attached only briefly still leaves its cell size behind: an image arriving
  after it detached is laid out as if it were still there. (The demo's case: a client was attached,
  but only for a moment.)
- Applications asking for sizes get one consistent, real answer: `CSI 14 t`, `CSI 16 t`,
  XTSMGRAPHICS sixel geometry, and the PTY's `TIOCGWINSZ` pixel fields all derive from the same
  cell size.
- The daemon never crashes on an image, whatever the cell-size state.

## Non-goals

- Re-laying out images already in the grid when the cell size changes. An image's cell extent
  moves the cursor and scrolls the grid, so it is committed when the image arrives; it cannot be
  revised per client or after the fact.
- Per-client layouts. There is one grid per session, so there is one cell size per session.
- tmux control-mode (`tmux -CC`) clients reporting a pixel size. They have no way to; they simply
  never change the remembered cell size.

## Anti-goals (would make this a failure even if it "works")

- An invented cell size used while a real one is known or remembered. The fallback exists only for
  a daemon that has never seen any client.
- Size replies that contradict each other (PR #2212 had `CSI 16 t` say 10×20 while `CSI 14 t`
  said 0×0).
- Any path that divides by a zero cell extent.

## Decisions (the user's)

1. **Remember the last cell size.** A terminal keeps the last cell size a client reported and lays
   images out on it while no client is attached.
2. **The latest client wins.** When several clients are attached with different cell sizes, the
   most recent one decides. The other clients adapt: they draw the same laid-out image stretched
   into their own cells. They need no new code for this — `ScreenMirror` already rasterizes each
   placement against the client's own cell size with the placement's alignment and resize policy.
3. **Fallback: 10×20, configurable.** Before any client has ever reported a cell size, images are
   laid out on 10×20 pixels — the VT340's text cell (800×480 screen, 80×24 characters), the
   machine sixel was designed for. The value is a **global** config setting.

## Decisions (mine — check these)

- **"Latest" follows `daemon.size_policy`, per session.** That setting already decides whose grid
  size wins (`vthost::ClientSizePolicy`, default `Latest`). A session's cell size comes from the
  same report that decided *that session's* grid, so a session never mixes one client's
  rows/columns with another's pixels. There are two such winners today, and the cell size follows
  whichever applies: the pane's refinement winner (`refinedSize`, the latest eligible `ResizePane`
  reporter) if the pane has one, otherwise the client-area winner (`resolveAuthoritativeArea`).
  So the cell size is stored **per session**, next to the refinement, not host-wide. Under the
  default this is exactly decision 2. Where the resolved grid is an intersection/union that no
  single client owns (`smallest`/`largest`), take the latest reporter's cell size. If the winning
  report carries no cell size (a tmux control-mode client, which also calls `applyClientSize`),
  the session keeps the cell size it has.
- **Daemon-wide memory, not only per session.** Besides each terminal keeping its own last cell
  size, the daemon remembers the last cell size any client reported. A session created while no
  client is attached (e.g. by a Claude session through the control API) starts from that, and only
  falls back to the configured default if the daemon has never seen a client.
- **It survives restarts.** The remembered daemon-wide cell size is written with the
  `daemon.restore` snapshot and carried across a live upgrade in its own `HandoverImage` field
  (precedent: `defaultColorPreference`), since a daemon with no sessions at upgrade time has no
  per-session slot to carry it in. Per session, the handover's existing `pty.pixels` slot
  (`vtpty::InheritedPty`, filled by `UnixPty::handover()` from the last pixel resize) starts
  carrying a value by itself once the PTY is resized with pixels.
- **Fix the restore division while there.** `HandoverRestore.cpp:231` derives the cell size as
  `pty.pixels / totalPageSize()`, but the PTY is given `cell × mainDisplayPageSize`, so the cell
  height is wrong whenever a status line is shown. Divide by the main display page size, and make
  the test at `HandoverRestore_test.cpp:481` check the exact value rather than `height > 0`.
- **The first session's PTY is honest too.** The client's cell size also rides on the Hello's
  `clientArea` (`NativeSession.cpp:2278`), which is applied before the first spawn, and the PTY
  factory (`SessionHost.hpp:91`) takes the pixel size as well as the page size, so a PTY is born
  with real `TIOCGWINSZ` pixels.
- **Config key:** `daemon.default_cell_pixel_size: { width: 10, height: 20 }` in the global
  `daemon:` section. Both extents must be positive; a zero or negative value is a config error
  reported the way other invalid `daemon:` values are, and the built-in 10×20 is used.
- **GUI-local terminals are unchanged.** A non-daemon GUI terminal before its first layout keeps
  #2208's behaviour (drop and log). Whether that window is reachable at all is an open question for
  the builder (see below); this design does not change it.

## How (decided)

- **Protocol.** The client reports its cell size in pixels with its size reports — `ResizeRequest`
  and `ResizePane` — or in a new PDU sent alongside them; and again whenever it changes (font size,
  DPI change, moving to another screen). Per `Pdu.hpp`'s own rules this is **not** a version
  step: the fields are appended to the existing bodies and a decoder defaults a missing trailing
  field (precedent: `LayoutState::servedEdit`); a new PDU, if chosen, is gated by a `Capability`
  row. A peer that sends no cell size is treated like a tmux control-mode client: it does not
  change the remembered value.
- **Daemon.** `SessionHost` applies the winning cell size under the terminal lock, in the same
  place it applies the grid size. This needs a **new vtbackend seam**: "the cell size changed,
  the grid did not" — set the cell size and push the new pixel size to the PTY (`TIOCSWINSZ`),
  *without* the side effects of `Terminal::resizeScreen` (margin/selection reset, full resync).
  `setCellPixelSize()` alone is a bare member write that never reaches the PTY, and
  `SessionHost::resizeLocked` early-outs when the grid is unchanged, so without the seam a font
  zoom would not reach the daemon at all — and routing it through `resizeScreen` would reset a
  running application's scroll margins on every zoom. A report that changes both grid and cell
  size goes through `resizeScreen(size, pixels)` as today.
- **vtbackend.** Keep `cellPixelSize()` meaning "what we were told, or zero". The daemon now
  always has a non-zero one (told, remembered, or configured default), so its images never take
  the drop path. #2208's drop-and-log stays as the guard for any terminal that genuinely has no
  cell size, so no path divides by zero.
- **Replies.** All size replies keep reading `cellPixelSize()`; since the daemon's is now real or
  the configured default, they agree with each other and with the layout.

## Salvage from PR #2212

Close #2212. Worth carrying over:

- Its regression tests for sixel, GIP and kitty extents, adapted to "laid out on the cell size the
  terminal was given".
- Removing kitty's `std::max(cell, 1)` clamp, which on a 1×1 cell made a 20×40 image cover 20×40
  cells — replace it with the same drop-while-unknown guard the other paths use.

Not carried over: `Terminal::imageCellPixelSize()` and the hard-coded 10×20 inside vtbackend.

## Testing

- vtbackend: a terminal given a cell size lays out sixel, GIP and kitty images on it, and the four
  size replies agree.
- vthost (headless, `daemon` label): a client reports a cell size → the session's terminal and PTY
  have it; the client detaches → a sixel still lays out on it; a second client with a different
  cell size attaches and reports → new images use the second one's; a session created with no
  client attached uses the daemon-wide remembered value, or the configured default if none.
- Restore and live upgrade: the remembered cell size survives both.
- Config: `daemon.default_cell_pixel_size` parses; invalid values are rejected.
- The original crash: a sixel printed into a daemon pane that no client ever attached to lays out
  on the default and does not crash.

## Left to the builder

- Whether the cell size rides on `ResizeRequest`/`ResizePane` or in its own PDU.
- Where the client side gets its cell size from and when it re-sends it.
- How the daemon-wide value is stored in the restore snapshot.
- **Open question:** can a non-daemon GUI terminal actually receive output before its first
  layout? If it can, raise it separately; it is out of scope here.
