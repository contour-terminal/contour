# Daemon mode

Contour has always multiplexed — tabs and split panes, in one process. Daemon mode moves
that ownership OUT of the window: a Qt-free daemon process owns the sessions and their
layout, and thin clients connect over sockets, so the sessions outlive any client showing
them. Two protocol families are served, and the same code also speaks the client side of
both:

- **tmux control mode** — external tmux-aware tooling (iTerm2-style clients,
  scripts) drives Contour's tabs and panes over the tmux 3.7b line protocol.
- **the native cells+deltas protocol** — the server emulates, the client
  renders: per-line grid deltas addressed by stable row ids (wezterm's model).

The module stack, bottom-up: `src/coro` (C++23 coroutine primitives, a
verbatim port of endo's `src/coro` at commit `178cb496`; the re-sync recipe is
in `src/coro/README.md`) → `src/net` (reactor `EventLoop`, sockets, buffered
readers, write queues) → `src/vthost` (the daemon, both protocol servers,
and both client engines). The GUI may depend on `vthost`, never the
reverse.

## One session, two taps

A hosted session is `{Pty, vtbackend::Terminal}` owned by the daemon's
`SessionHost`, pumped by a dedicated blocking-read thread exactly like the
GUI's `TerminalSession::mainLoop`. Everything else — sockets, protocol state,
the `vtworkspace::SessionModel` — is confined to one reactor thread; the pump
threads marshal onto it via `EventLoop::post()`.

Each session's output is observable at two points:

- the **byte tap** (`TappingPty`, a `vtpty::Pty` decorator): raw PTY bytes
  before the parser consumed them. This feeds tmux control mode's `%output`,
  which is byte-exact — the attached client emulates. Sixel/iTerm2/Kitty image
  bytes, OSC 66 and OSC 8 pass through untouched.
- the **cell tap** (`Grid::forEachLineChangedSince`): per-line deltas after
  parsing. This feeds the native protocol — the client runs no parser.

The pump loop flushes the terminal's queued replies (DA1, DSR, OSC color
queries) back into the PTY after every processed batch. This is the
daemon-side stand-in for the GUI's `screenUpdated → flushInput` hop; without
it an interactive shell blocks in its startup terminal probes forever.

## tmux control mode (pinned: tmux 3.7b)

The server (`vthost/tmux/ControlSession`) speaks the line protocol exactly
as tmux 3.7b does — the installed oracle and the reference tree agree, and the
layout codec's conformance tests drive real `tmux select-layout`:

- guarded command responses `%begin/%end/%error <time> <number> <flags>`, the
  same triple on all three lines, flags bit 0 = client-originated;
- `%output` escaping only bytes < 0x20 and backslash as exactly three octal
  digits (0x7F and ≥ 0x80 pass raw);
- tmux's dual-queue ordering: a notification never lands inside an output
  block and never before output queued ahead of it;
- pause/continue flow control with tmux's byte budget
  `(high − buffered) / panes / 3`, floored at 32; `refresh-client -f
  pause-after=N` switches emission to `%extended-output %N <age-ms> : …`;
- layout strings with the rotate-add checksum; ingest collapses tmux's n-ary
  containers into right-leaning binary chains (round-trips compare trees, not
  strings);
- `refresh-client -C` resizes within tmux's `[1, 10000]` bounds; the host then
  reprojects every pane's PTY size through `vtworkspace::layoutInCells`, so
  advertised layouts and actual shell dimensions never drift.

The client side (`tmux/ControlModeParser` + `tmux/TmuxGateway` +
`tmux/TmuxClientModel`) attaches to a real `tmux -C` (or to this project's own
daemon — the loopback tests validate both halves against each other): recovery
mode until the opening guard, FIFO command correlation, and a per-pane replay
`vtbackend::Terminal` fed by the raw `%output` stream. History arrives once
per pane via `capture-pane -peqJ`.

**Inherited limitation:** tmux serializes only text and SGR in capture-pane —
no images. A client attaching *after* an image was drawn cannot recover it
from replay; images work while attached (live `%output` carries the bytes and
the replay terminal parses them natively). Mitigation, if ever, goes through
the native protocol — never through capture-pane.

## The native protocol (cells + deltas)

The wire format (`vthost/proto/`) is wezterm's codec shape without the
dependencies: a frame is `varint taggedLength, varint serial, varint ident,
payload`, where the tagged length's low bit is a reserved compression flag
(never set, always rejected — compression can arrive later without a version
break). `serial == 0` marks unsolicited server pushes. Unknown idents decode
to `Invalid{ident}` — data, not an error — so newer peers keep talking.
A `ClientHello/ServerHello` version handshake precedes everything.

The handshake is an **exact match**, not a negotiation: a peer one version away is refused, and the
two ends must therefore be the same build. The refusal is deliberately shaped like a token mismatch
(same `ServerHello`, same drop), so an attacker probing it learns neither the version nor whether its
token was right; the price is that a user who upgraded and left an old daemon running gets no
diagnosis from the wire.

**While the protocol is unreleased, `CodecVersion` stays 1 and is NOT bumped per change.** Nothing
outside this source tree speaks it, so there is no peer for a version number to be compatible with;
the counter had reached 13 above a changelog of twelve revisions no reader could ever have observed.
Fields are added, removed and renamed without touching it, and the price — two builds that pass the
handshake and then disagree about the bytes — is paid by rebuilding the daemon and its clients
together. Once the protocol ships, versioning resumes and every wire change needs a new number.

The same reasoning applies to `PduType`: the catalog is kept **contiguous** and renumbered freely, so
a retired PDU leaves no gap. A gap exists to keep a tag from being reused while a deployed peer still
remembers the old meaning; with no deployed peer it is archaeology. Tags become stable at release too.

Two limits ride here as well, both applying in BOTH directions: `MaxFrameSize` bounds a declared
payload, and `MaxGridExtent` bounds an announced or proposed grid — an announced one because the
client sizes a repaint buffer from it. Integral fields are decoded with a range check rather than a
narrowing cast, so an out-of-range value is a `MalformedPdu` instead of a plausible small one.

### Why there is no `CloseSession`/`Unfollow` verb

Session removal is covered from both ends without one, and the shape is worth stating because the
absence looks like an oversight:

- **Client → daemon** is `ClosePane`, authored where the *intent* is known — the manager's
  pane/tab-close paths, through `SessionFactory::requestRemoteClose` — never inferred from a
  destructor. A PTY dies on a pane close, a window close, an app quit and a lost connection alike,
  so `contour::SessionEnd` makes every teardown entry point state which it is. Getting that wrong is
  silent in both directions: authoring on a detach kills sessions the user meant to keep, and *not*
  authoring on a close leaves them running headless (the bug this replaced — `NativeController::
  unbind()` was the only author, and nothing destroys a `TerminalSession` until app teardown, so no
  close was ever sent).
- **Daemon → client** is the re-pushed `LayoutState`: every model change fans out through each
  connection's layout observer, and the client reconciles structurally rather than per session id
  (`applyRemoteLayout`'s subtractive pass). A window whose last tab closed still reports, as an
  empty tab list, so removal is never signalled by silence.
- **Per-session server state** is dropped on `SessionStreamEvents::sessionClosed`, which erases the
  session's follow state and pending-delta entry — a dead session cannot accumulate work.

Adding the verb later is not a compatibility event, which is why it is not rushed in ahead of a
release: a new tag decodes to `Invalid{ident}` on an older peer, and the exact-match handshake above
means both ends are the same build regardless. What genuinely costs a version bump is changing an
existing PDU's fields.

### Where a split ratio lives

**In the daemon's model, or the user loses it.** A client rebuilds every tab from `LayoutState` on
attach, so a divider the daemon never heard about comes back at the ratio the split was *created*
with. Both client-side mutators — the QML divider drag and the `ResizePane` keybinding — converge on
`vtworkspace::ModelEvents::paneRatioChanged`, and that one callback is where `ResizeSplit` is
authored (`TerminalSessionManager::paneRatioChanged` → `SessionFactory::reportSplitRatio`).

`ResizePane` is the near miss worth naming: it carries the CELLS one pane is drawn at, and
`SessionHost::applyPaneSize` documents that the next re-projection recomputes those from the ratio
and discards them. A grid is a consequence of a ratio, not a place to keep one.

Two details the mechanism rests on:

- **The split is named by two of its leaves**, not by a pane id — matching `SplitPane`/`ClosePane`/
  `ResizePane`, all session-keyed. The server resolves their lowest common ancestor
  (`vtworkspace::Pane::lowestCommonAncestor`), which is the same split node in either tree. *Any*
  leaf from each side works, so neither end has to agree with the other on how to pick one.
- **Ratios are compared in WIRE units** (`proto::toWireRatio`, 1/10000), on both ends. The daemon
  ignores a report that re-asserts the ratio it already holds — a client re-asserting its layout on
  attach would otherwise cost every attached client a re-projection and a fresh `LayoutState` — and
  the client's reconciler writes only when the quantized values differ. Comparing decoded doubles
  instead would see a difference on every push that no write can remove.

`applyRemoteLayout` therefore has a third pass, after the additive and subtractive ones and on the
settled tree, that re-flows an already-realized tab when another client moves a divider. It pairs
each wire tab with the local tab that matches it leaf-for-leaf, so a tab caught mid-reconciliation is
skipped rather than written onto. That pass is the shape to generalize when a SECOND attribute needs
the same treatment: `WireTab` already serializes `activePane`, `zoomedPane`, `title` and `color`, and
none of them are reconciled yet — at that point one lockstep `reconcile(wire, local)` walk subsumes
all three passes, and the outbound half wants `NativeController` as a second `ModelEvents` observer
rather than a hand-written verb per attribute.

Line PDUs carry the full per-cell renditional state (grapheme cluster extras,
widths, OSC 66 scales and packed extras, hyperlink ids, colors, the 20
CellFlags bits) plus per-batch side tables: hyperlink id→URI (sent once per
connection on first reference, immune to the server-side LRU evicting the id
later) and image-covered cells. Image pixels are never inline: clients fetch
by stable image id (`FetchImage` → `ImageData`/`ImageGone`), served from
`ImagePool`'s id→weak_ptr index — eviction stays refcount-driven.

Full per *sent* cell, that is — a row does not send the trailing columns that already
match its own fill, and a uniformly-filled row sends no cells at all. A shell line
occupies a fraction of the grid's width, so across a scrollback that padding is the
bulk of a snapshot, at ~24 bytes per unused column. **This makes the fill a shared
contract rather than a rendering detail**: an absent column means a default cell wearing
the row's `fillForeground`/`fillBackground` and otherwise SGR-reset (the fill carries
neither an underline color nor flags, so a row whose fill has either has nothing to
omit), and *every* client render path must paint that fill across the row before writing
cells over it. A path that reconstructs the cells alone truncates every filled region at the
last column that happened to be sent — which is why `applyWireLine` clears the row to
`fillAttrsOf(row)` before writing any cell, and why the same function serves the page,
the history and the host-writable status line.

### Why the client does not render escape sequences

A thin pane must be indistinguishable from a local one — same behaviour, same rendering — and for
a while the client got there by re-serializing each delta into VT bytes and feeding them to the
session's own parser. That was appealing: one mechanism, no new API, and every feature built on
`vtbackend` worked because a real `vtbackend` was doing the work.

It cannot reach parity, for a reason no amount of extra sequences fixes: **the escape-sequence
vocabulary cannot express everything a grid holds.**

- `LineFlag::Wrapped` has exactly one producer in the whole engine — a real autowrap, in
  `Screen::crlfIfWrapPending`. No sequence sets it. A mirror that cannot say "this row continues
  the one above" has no logical lines, so reflow on resize, double-click selection, search across a
  wrap and the shell's semantic marks all worked from wrap state it had invented.
- `Line::promptEndOffset()` / `commandEndOffset()` have no spelling at all. The terminal takes them
  from wherever the cursor stood when the shell spoke OSC 133, and `Line::reset()` — which every
  row erase goes through — clears them, so even a re-emitted OSC 133 would need re-asserting after
  every repaint.
- Reproducing the marks with real OSC 133 would additionally feed the client's
  `SemanticBlockTracker` command text and exit codes it does not have.
- The underline STYLES had no distinct representation either, because the shared flag→SGR table
  held one integer per flag and could not spell `4:3`.

So the client writes the fields the wire names into the fields they name. That has no ceiling, and
it is cheaper: an update no longer costs ~25 bytes of SGR per cell plus a full re-parse.

**What that obliges the populator to do by hand.** Bypassing the parser means bypassing the
bookkeeping `Screen` performs, so every write goes through the narrowest engine primitive that
maintains it rather than at the arrays underneath — cells via `writeCellToSoA` (the `trivial`
render fast-path flag, the grapheme-cluster pool, image-fragment replacement), scrolling via
`Screen::scrollUp` (the cursor iterator, plus `onBufferScrolled` for the viewport, the Vi cursor
and any live selection), the cursor via `Screen::moveCursorTo`, and the frame via
`Terminal::screenUpdated` — called outside the terminal lock, exactly where
`Terminal::processInputOnce` publishes a parsed chunk.

Where the wire and the engine disagree, the **sender wins**: `applyWireCell` restores `width` and
`scale` after those calls, because `writeCellToSoA` resets scale to 1 and `appendCodepointToCluster`
re-measures the cluster. The authority on how many columns a cluster occupies is the terminal that
ran the emulation, not the one displaying it.

### The parity oracle

Parity is a claim about every cell and every line of two grids, which nobody can settle by reading
code. `vthost/testing/GridParity` compares them field by field, and `[parity]`-tagged cases in
`client/ScreenMirror_test.cpp` drive one content shape each through the real server + client +
mirror loop.

Three things make it trustworthy:

- Both sides are read through the daemon's own `toWireLine`, so the comparator is exhaustive over
  the protocol's vocabulary by construction — and rows are expanded to full width first, so a blank
  row and a materialized row of identical appearance do not read as different.
- Fields are compared from **tables** (`LineFields`, `CellFields`), so a field added to the wire is
  one row here rather than a chain of `if`s that can silently omit it. Images are the exception, and
  the reason the tables are not the whole story: they travel in a side table beside the rows, so
  a comparator built only on wire rows is blind to them — hence `describeImage`, read off the
  storage.
- Each probe states what the SERVER must be holding (`serverLineFlags`, `serverHasCellFlag`,
  `serverHasImage`). Two grids equally EMPTY of the thing under test pass trivially, and a probe
  that never engaged its feature looks exactly like one that proved parity for it.

Comparisons resolve the page through `Terminal::displayedPageIndex()` — the page the daemon
actually serializes. Not `currentScreen()` (the page VT output targets, which a decoupled display
or an NP/PP flip makes a different page), not `activeDisplay()` (which follows DECSASD), and not
`isAlternateScreen() ? alternateScreen() : primaryScreen()` (which reads the xterm alt page, where
DEC pages 1..14 do not live).

The resize probes carry the load-bearing assumption: reflow runs **twice**, once per grid, with
nothing synchronizing them. They agree only because both grids were faithful copies and reflow is
deterministic in (content, flags, old size, new size) — so that agreement is measured, in both
directions, rather than argued.

### The send queue: what a bound may and may not conclude

`net::WriteQueue`'s byte bound exists to detect a peer that stopped draining, and two rules
keep it from firing on anything else. Both were learned the hard way — the daemon used to
disconnect perfectly healthy clients on a window resize.

- **The bound governs the backlog, never a single frame.** A frame landing on an empty
  backlog is always accepted, however large. A full-grid snapshot of a deep scrollback is
  legitimately megabytes, and its size says nothing about the peer; refusing it made a
  session with enough history permanently unattachable, a cliff neither end could avoid.
  The in-flight frame is still counted as owed to the peer (`queuedBytes()`), just no
  longer governed by the bound (`backlogBytes()`) — it can no longer be superseded or
  refused.
- **A snapshot supersedes what it re-describes.** Frames are tagged with their session, and
  pushing a snapshot drops that session's unwritten frames (`dropTagged`) instead of
  stacking on top of them. Without it a burst of resyncs — a window drag, or an attach
  immediately followed by the client asserting its area — queues whole grids until the
  bound trips, for frames the client's mirror would have discarded on arrival anyway.

The complement to both lives upstream: a resync is only pushed when a grid actually moved.
`SessionHost` reports that (`SizeChange`) rather than letting callers assume a resize
request changed something, because clients re-assert an unchanged client area on every
attach, and each pane's grid is clamped before it is applied — so both "different request,
same grid" and "same request, discard a per-pane refinement" are real cases.

### Stable row identity in the grid

The delta source lives in `vtbackend` (added by this work, guarded by the
usual terminal lock, one byte store per mutation on the hot paths):

- every `Line` carries a dirty bit set by all mutating entry points, and copy/
  move *assignment* dirties the destination row (margin scrolls move whole
  lines between rows);
- `Grid` names physical rows by **stable ids** maintained exclusively inside
  the ring-rotation primitives: scrolling changes a row's `LineOffset`, never
  its id. The eviction floor is monotonic and deliberately not derived from
  `historyLineCount()` — at-capacity scroll-down wraps destroyed page rows
  into the oldest history slots without resetting them, and a derived floor
  would re-validate evicted ids against garbage;
- wholesale rebuilds (resize/reflow, history-limit change, reset) bump a
  **generation** instead; ids are only meaningful within one generation;
- `finalizeRevisions()` stamps dirty lines with a batch seqno lazily —
  only when a consumer queries; an idle or daemon-less terminal pays nothing.
  `forEachLineChangedSince(cursor, f)` self-finalizes, reports rows whose
  revision passed the cursor, and answers `ResyncRequired` on a generation
  change — a resync is a `forEachValidLine()` snapshot, never a "changes since
  seqno 0" replay, because post-rebuild rows legitimately keep revision 0.

Perf gate (measured, clang-release, Callgrind): total Ir +0.30 % against the
pre-retrofit baseline (acceptance was < 0.5 %), `writeTextToSoA` and
`Screen::writeText` byte-identical, wall-clock parity.

### Transport and clients

`NativeSession` (server) pushes an attach snapshot (SessionState + a snapshot
Delta per session), then 20 ms-debounced deltas off the host's screen-updated
signal. Deltas also carry the currently-SET DEC private modes of a
single-sourced mirrored-mode table (`vthost/MirroredModes.h`: cursor keys,
keypad, backarrow, bracketed paste, focus, cursor visibility) —
everything a client needs to encode INPUT correctly; a pure mode flip pushes
even when no cell changed. Output-side modes (autowrap, origin, margins) stay
local by design: the server's emulation already applied them to the cells.

**Every mode in that table must be an INDEPENDENT boolean**, because the client replays the set by
applying its rows in order — so two rows writing one underlying value would let the later one
overwrite the earlier one's answer. Input state that is not an independent boolean rides its own
field instead, pull+diffed like every other live value: the Kitty keyboard flag stack's top
(`CSI = flags ; 1 u`), xterm's modifyOtherKeys level (`CSI > 4 ; level m`), and the **resolved
mouse state** — protocol, coordinate encoding and wheel mode.

The mouse is the case that proves the rule rather than illustrating it. Nine DEC modes write only
those three values (9/1000/1002/1003 the protocol, 1005/1006/1015/1016 the encoding, 1007 the
wheel), so a mode SET cannot express which spelling won: replaying `{1000 on, 1002 on, 1003 off}`
in table order ends with the protocol OFF, because resetting *any* protocol mode clears whichever
is active — xterm's rule too (`really_set_mousemode`). The visible symptom was that the mouse was
dead on exactly those clients whose mirror was built while an application was already tracking,
which read as an attach-order bug and was really this. `SessionState`/`Delta` therefore carry
`mouseProtocol` (as its DECSET number, 0 for none), `mouseTransport` and `mouseWheelMode`, and
`ScreenMirror` writes them straight through `Terminal::setMouseProtocol`/`setMouseTransport`/
`setMouseWheelMode` — after the modes, since DECCKM rewrites the wheel mode on the alternate screen
and enabling a protocol resets it.

While fixing that, `vtbackend` gained the xterm rule it was missing for the four coordinate modes:
they are one mutually-exclusive setting, so a reset is effective *only against the matching mode*
(xterm `charproc.c`, `srm_EXT_MODE_MOUSE`). Contour treated them as four flags, which additionally
made `CSI ? 1005 l` and `CSI ? 1015 l` *enable* their encodings — they ignored the set/reset bit
outright.

**A snapshot must state live state outright, defaults included.** Live fields
(title, cursor shape, cwd, default colors, status display, the two input-encoding
protocols) are pull+diffed against a per-connection `FollowState`; on a snapshot
they travel in `SessionState` instead of a Delta changed-flag, and the server then
records them as sent. A client replay that re-emits such a field only when it is
non-default therefore drops any value that returned to its default inside a
snapshot — permanently, because the server will not mention it again. That is not
hypothetical: a shell popping its `CSI u` flags in the same batch as a child's
alt-screen entry left clients CSI-u-encoding keys for an app that had stopped
asking, so `Ctrl+A` reached tmux as literal `CSI 97;5u` text.

**The daemon is the authority for scrollback, so it must have some.** A bare
`vtbackend::Settings` leaves `maxHistoryLineCount` at `LineCount(0)`, and with no
history the daemon cannot merely fail to replay it — `Grid::scrolledOutDepthSince`
clamps to zero, so rows that scrolled off between two deltas cannot be *named*, and
a client mirrors them as blank scrollback. `DaemonConfig` therefore defaults it
non-zero, and `contour daemon --config/--profile` derives the emulation half of its
session settings from the same `config::emulationSettings()` table the GUI's session
factory uses (a client auto-spawning a daemon forwards both flags). When a burst
still outruns what the floor can name, `pushDelta` promotes it to a snapshot: rows
the daemon can no longer name become an honest absence rather than fabricated blanks.

**The invariants live at the mechanism, not at the policy above it.** `vthost::
hostedSessionSettings` (`vthost/SessionSettings.h`) normalizes any settings a hosted session is
about to be built with, whatever produced them — the daemon's profile, a client's stated preference,
or a bare `vtbackend::Settings`. `SessionHost` runs it in its constructor and again in
`seedSession`, so no caller can open the defect back up. Two things it asserts: a zero scrollback
becomes `DefaultSessionHistoryLineCount` (a *small* one is left alone — the default is a default,
not a floor), and `goodImageProtocol` is forced on, because that knob is maturing toward
non-configurable and daemon mode adopts the end state now. The latter is also why it is absent from
the wire: carrying a setting about to disappear would buy one release of fidelity and cost a wire
break to remove.

**Emulation is owned by the daemon; a session inherits the profile of the client that CREATED it.**
`ClientHello` optionally carries `WireSessionSettings` — the emulation half of the client's resolved
profile (history depth, reported terminal identity, reflow, grapheme clustering, frozen modes, image
registers; never the page size, which `ResizeRequest` negotiates). The daemon applies it over its own
settings, validating every field, and hands the result to the sessions *that connection* creates:
`CreateTab`, `SplitPane`, `NewWindow`, and the first session an attach to an empty daemon spawns.
Sessions that already exist are never re-emulated, which is what makes two clients on different
profiles conflict-free rather than a race — an application that already read DA1 cannot be told its
terminal changed identity. Both `contour daemon` and `contour client` resolve those settings through
the one `config::resolveEmulationSettings` helper, so a warm daemon and the client attaching to it
cannot disagree about what a profile means.

Transient session events — bell, desktop notification, OSC 52 clipboard write — are one PDU each
(`SessionBell`, `SessionNotify`, `SessionClipboard`), projected for consumers as
`proto::SessionEventPdu`. They were briefly a single PDU with a `kind` byte and two anonymous
strings, which forced every payload into the union of all of them and left nothing but a comment
saying which field meant what.

`client/NativeClient` mirrors sessions into a `RemoteScreen` — plain data any
frontend can render — and `contour client` is the GUI frontend on top of it.
Sessions survive detach; a reattach replays history. The daemon serves the native
protocol on `<control-socket>-native`.

## The GUI seams (`contour client`, `contour client --tmux`)

The display stack consumes a session exclusively through
`TerminalSession::terminal()`, so a REMOTE session is an ordinary
`TerminalSession` whose `vtbackend::Terminal` sits on a `vtpty::ChannelPty` —
the blocking-read, sink-driven in-memory Pty (promoted from the GUI test
fixture, so its semantics were already test-proven). Nothing in the
display/render/input machinery learns the session is remote:

- **Input**: every keystroke funnels through the one `_pty->write()` in
  `Terminal::flushInput`; the ChannelPty's write sink posts it onto the
  controller's reactor (native: `NativeClient::sendInput`; tmux:
  `send-keys -H` hex batches — the quoting-proof channel for encoded input).
- **Resize**: `Terminal::resizeScreen`'s `_pty->resizeScreen()` fires the ChannelPty's resize sink,
  which `NativeController::reportPaneGeometry` coalesces onto the GUI thread and
  `flushGeometry` turns into a COMPOSED `ResizeRequest` (the whole content area, solved back up the
  pane tree) plus a `ResizePane` per pane; the tmux path sends `resize-pane -x -y`. The client area
  is a report of what this client can display, not an instruction — @see "One grid, several
  clients" for how the daemon resolves one from all of them.
- **Output, native path**: `client/ScreenMirror` populates the session's
  `vtbackend::Terminal` GRID directly from the delta stream — no bytes, no
  parser — so scrollback, selection and search work natively on mirrored content.
  History enters by scrolling rows through the page (real scrollback, no filler);
  hyperlink ids are translated against the connection's side table into the
  mirror's own store. `vthost/GridWire`'s `applyWireLine` is the exact inverse of
  the `toWireLine` that produced the row, sitting beside it so the round trip
  cannot quietly lose a field. See "Why the client does not render escape
  sequences" below for what forced this.
  Because that history is the MIRROR's, a full replay must not erase it unless
  the server discarded its own (`LocalHistory::Keep` vs `Discard`): the mirror is
  routinely deeper than the daemon, so rebuilding from the server's remaining rows
  truncated the user's scrollback on every resize, page flip and resync — in
  practice, every time a full-screen app started or quit. A generation bump alone
  does not earn a discard, because a RESIZE bumps it: this terminal reflowed the
  same rows at the same moment, so its history is the reflowed truth. Only a first
  replay, a reset or history-limit change (a bump with the size unchanged), and a
  server-side `clear` (detected as the floor outrunning the viewport advance) discard.
  Two rules keep that decision honest. **Neither the floor nor the generation may be
  compared across a page change**: the 16 pages are distinct grids whose ids and
  generations advance independently, so a primary↔alternate flip was being judged on
  numbers from an unrelated id space — a spurious discard where they differed, and no
  rebuild at all where they collided. And **`Keep` preserves history, it does not
  decline to have any**: where the target page holds no local scrollback the replay
  streams the snapshot's rows in regardless, because there is no second copy to make.
  Without that, a client attaching while the session sat on the alternate screen never
  materialized the primary's pre-attach scrollback, though the wire had carried it.
  The retention bound is the client's own `history.limit`
  (`RemoteScreen::historyKeep`, stamped by `NativeClient` from the `ClientHello`'s
  settings), not a constant: a hardcoded ceiling silently contradicted a user who had
  configured deeper scrollback — the daemon served the rows and the mirror dropped them.
- **Output, tmux path**: the raw `%output` bytes ARE VT — `TmuxClientModel`'s
  injectable `PaneSink` feeds them (buffering capture-pane replay until the
  local pty binds). The replay is `capture-pane -peqJ -S -`; `-S -` is what reaches
  past the visible page into the pane's history, and without it this direction
  replayed only the current screen while our own control-mode server was already
  answering `-S -` for tmux clients.

Each controller (`contour/remote/NativeController`,
`contour/remote/TmuxController`, sharing `contour/remote/RemoteController`)
runs the Qt-free client engine on its own reactor thread (`ReactorThread`) and
doubles as the app's `SessionFactory`: the manager's creation entry points ask
`canCreateSession()` first, so a "+" click inside a mirror window cannot spawn
a stray local shell. The app's factory is permanently a
`RoutingSessionFactory`; attach mode switches the route, never the manager's
reference. v1 mapping: one tab per daemon session; tmux windows become tabs
and additional panes split the tab.

### One grid, several clients

Several clients may attach to one daemon, and they share **one** grid per session — per-client
server-side viewports were considered and not built (the F8 decision), since the alternative costs
a viewport per follower in `NativeSession` plus a per-client delta cursor. Sharing one grid makes
two questions unavoidable, and both used to be answered by accident.

**Which size is it?** Not "whichever client reported last", which is a race rather than a policy:
two differently-sized clients took turns resizing every application on the daemon. `SessionHost`
keeps each attached client's reported area and resolves ONE authoritative area from all of them
under `vthost::ClientSizePolicy` (`--size-policy`, default `latest`, mirroring tmux's `window-size`
in both the values and the default). The resolution is a pure function of the reported set, which
is what makes it settle: re-reporting the same areas in any order yields the same answer.
`smallest`/`largest` combine **per axis**, because two clients can each be the wider and the
shorter one. The registry is keyed by the client's stream subscription, so a detaching client's
area stops counting — otherwise a client that is gone pins every application to its dimensions for
the daemon's life. tmux's fourth value, `manual`, is deliberately absent: it needs a runtime verb
to set the size with, and nothing here would drive one.

**Who hears about a change?** Everyone, which is the half that was missing: the resync used to be
issued by the connection that received the `ResizeRequest`, over its own follow map, so maximizing
one window left every other client rendering the old grid — `Terminal::resizeScreen` raises no
screen update, so nothing else would tell them either until that session next produced output.
`SessionHost::resizeLocked` is the one place any grid moves, so it announces each move through
`SessionStreamEvents::sessionResized` (with the terminal lock released, since observers take it to
snapshot). Every path inherits it: a client-area re-projection, a per-pane refinement, a layout
change. The connection that asked is no longer special.

**What a client that cannot show the grid does.** A client LARGER than the grid letterboxes — the
sub-cell remainder already renders as background padding, so the surplus simply is not drawn. A
client SMALLER than it shows a window into the grid, positioned by `contour::geometry::
viewportOrigin` — tmux's `tty_window_offset1` rule, ported as a pure function: centre the cursor
horizontally once it is past the viewport, keep it on the last visible row vertically, clamp so the
view never runs off the grid, and decide the two axes independently. The rule and its tests are in
place; wiring it into the render origin and the mouse hit-test is not yet done, so a too-small
client currently clips at the top-left instead of following the cursor.

## Socket conventions

`contour daemon`/`client` default to `$XDG_RUNTIME_DIR/contour/<label>`,
overridable per flag and `$CONTOUR_MUX`. The socket directory hardening
mirrors tmux exactly: 0700 directory, owner check, refuse **world**-rwx
(group is permitted — tmux's `TMUX_SOCK_PERM == 7`).

`vthost::daemonLogPath` derives `<socket>.log` beside the socket, as the default destination
for a daemon with no console to write to (`--background`, and anything `contour daemon-service`
installs). It sits with `nativeSocketPath`/`tmuxSocketPath` because it is the same kind of fact:
one place per label, derived rather than remembered.

**A path an installed daemon is given must be absolute, and resolved at install time.** A
registration outlives the shell that created it, and a service's `%TEMP%`/`%USERNAME%` need not
be the interactive user's — so a socket left to be derived at start time resolves somewhere the
user's own client never looks. `daemonServiceCommandLine` pins both the socket and the log file.

## Daemon lifetime

A daemon stops for one of three reasons: `SIGINT`/`SIGTERM` (POSIX), a console-control event
(Windows), or — with `--exit-with-last-session` — its last hosted session going away. All three
funnel into one `requestDaemonShutdown`, which closes every listener and then unwinds the loop.

`--exit-with-last-session` is **off by default**, because a daemon started by hand exists precisely
to outlive its sessions and still be there for the next client. `ensureDaemon` turns it **on** for
the daemon it auto-spawns: that one belongs to the client that needed it, and "make sure a daemon
exists" must not mean "leave one behind". The policy rides on the *daemon*, not on the spawning
client — a second client attaching inherits it, and the daemon ends when every session across every
client is gone. Reference-counting attached clients (tmux's separate `exit-unattached`) is not
implemented.

Sessions leave through exactly one funnel, `SessionHost::handleSessionExit`: a shell exiting (the
pump thread's posted completion), the native `ClosePane` PDU, or control mode's `kill-pane`. The
orphan reaps after a refused `createTab` deliberately do *not* fire `sessionClosed`, so a failed
creation is never mistaken for the last session ending.

Two details in `LastSessionWatcher` are load-bearing:

- **The trigger is edge-triggered, never a level check.** The daemon binds its sockets with *zero*
  sessions and only gains one when a client asks — attaching to a daemon with no tabs spawns the
  first session. "No sessions, therefore quit" would end the daemon before anyone could attach.
- **The decision is deferred one pump, then re-checked.** `sessionClosed` fires from inside
  `SessionHost`'s observer fan-out, which the shutdown would mutate as connection flows unwind. More
  importantly, buffered input is dispatched without touching the reactor (`coro::Task`
  tail-transfers; `PduPump` and `net::AsyncBufferedReader` return buffered frames and lines without
  awaiting), so a `ClosePane` followed by a `CreateTab` — or `kill-pane` then `new-window` —
  arriving in **one read** is fully dispatched before the loop regains control. The posted check
  therefore sees the batch's end state and the daemon survives.

Shutdown is abrupt by design: `requestStop` cancels each connection's parked read, so the graceful
epilogues (`NativeSession`'s `flushThenClose`, control mode's `%exit`) are skipped. Peers see a
clean FIN with no goodbye — which is exactly how a real tmux `kill-server` looks to them, and frames
are enqueued whole so none is ever truncated. Closing a unix listener also **unlinks its socket
file**, so the next `ensureDaemon` probe fails and spawns a fresh daemon rather than finding a dead
path.

One consequence worth knowing when shutting a daemon down by signal: a session whose shell is still
running blocks teardown, because `~HostedSession` joins a pump thread parked in a blocking PTY read.
The last-session path is unaffected (its sessions are already gone by then).

## Binary imsg IPC (the real tmux binary as a client)

The daemon's third endpoint, `<control-socket>-tmux`, speaks tmux's binary
client protocol — the rewritten libutil imsg tmux ≥ 3.6 uses: 16-byte
host-order `{type,len,peerid,pid}` header, `len` including the header with
its top bit marking one SCM_RIGHTS descriptor, masked length in [16, 16384],
`peerid`'s low byte carrying `PROTOCOL_VERSION` (8; a mismatch answers
`MSG_VERSION` and drops). The insight that makes this cheap: control mode
rides ON TOP of imsg. After the `MSG_IDENTIFY_*` handshake (a data-driven
table validates payload shapes exactly as the real server does) passes the
client's STDIN/STDOUT via SCM_RIGHTS and an attach-shaped `MSG_COMMAND`
arrives, the ORACLE-VERIFIED control-mode engine simply runs over the passed
descriptors (`net::adoptFd` + `net::combineHalves`); the imsg socket carries
only lifecycle. So `tmux -S <socket>-tmux -C attach-session` works with the
stock tmux binary — proven by an oracle test forking the real client, and
live against the shipped daemon.

Deliberate deviations from the real server, all verified against the
reference tree (`3.7b-617-g5ed5e360`):

- acceptance requires `CLIENT_CONTROL` and refuses `-CC`
  (`CLIENT_CONTROLCONTROL`) — we never render a full terminal client;
  rejections answer `MSG_EXIT` with a message, as tmux's own idiom does;
- the startup command is a table (`attach-session`/`attach`/
  `new-session`/`new`/empty ⇒ attach); arbitrary startup commands are not
  executed;
- the `%exit` line is SUPPRESSED on this path — the tmux client binary
  prints its own after its imsg loop ends — and the preamble guard pair is
  stamped flag 0 (the MSG_COMMAND-originated command is not
  client-originated in cmd-queue terms), while stdin-line commands keep 1;
- a detach drains the control stdout fully, THEN sends `MSG_EXIT`
  (mirroring `control_all_done` gating); `MSG_EXITING` is answered with
  `MSG_EXITED`;
- the socket's execute-bit "has attached clients" signal is not maintained
  (verified informational: no tmux client reads it before connecting).

`contour daemon --tmux-compat-socket LABEL` additionally binds tmux's own
discovery path `/tmp/tmux-<uid>/LABEL`, so a plain
`tmux -L LABEL -C attach-session` finds the daemon. Opt-in only: with the
daemon down, a `new-session` on that path would silently fork a REAL tmux
server onto it.

## Windows

The Win32 net backend serves AF_UNIX via `afunix.h` (Windows 10 1803+); the
socket's parent directory is created but NOT permission-hardened — NTFS ACLs
govern access, not POSIX mode bits. `runDaemon` serves the control and native
endpoints (no imsg: SCM_RIGHTS does not exist on Windows), with
`SetConsoleCtrlHandler` marshaling shutdown onto the loop. Sessions are
ConPTY-backed, exactly as the GUI's local ones are.

### Starting without a terminal

`contour daemon --background` relaunches its *own argv* with `--background=false` appended, through
`spawnDetachedDaemon`, then blocks on `spawnAndAwaitDaemon` until the native socket accepts —
so returning means "it is serving", not "the process started". Replaying its own tokens rather
than rebuilding them from parsed flags (`crispy::app::commandLine`) is what keeps a new daemon
option from being silently dropped by a second, forgotten spelling of the flag list.

The flag is *overridden* rather than filtered out of that argv, because `crispy::cli` spells it
four ways — `--background`, `-background`, the bare `background` (its natural style), each
optionally with a value in the `=VALUE` or separate-token form — and options are last-one-wins. A
token filter that misses a spelling makes the child spawn another detached child, for ever.

On POSIX the child calls `setsid()` and reopens its standard descriptors on `/dev/null` before
`execv`, which is what `DETACHED_PROCESS` buys the Windows branch: a daemon left in the invoking
shell's session and foreground process group takes the terminal's Ctrl-C (killing every hosted
session) and dies outright on the SIGHUP of a closing terminal, which `runDaemon` does not block.
Its diagnostics go to `--log-file`, which this path always supplies.

`contour daemon-service install|uninstall|start|stop|restart|status` registers that daemon with
the OS. Two mechanisms hide behind the verbs, because on Windows **no single one does both
halves** of what is wanted (`vthost/ServiceControl.h`):

| `--start` | Backend | Trigger | Elevation | Password |
| --- | --- | --- | --- | --- |
| `logon` *(default)* | Task Scheduler 2.0 (COM) | the installing user's logon | no | no |
| `boot` / `manual` | SCM service | boot / on demand | yes | **yes** |

The public service-trigger set has **no logon trigger** — `SERVICE_AUTO_START` means *boot*, in
session 0 — and `CreateServiceW` under a named account needs that account's password, where a
task registers with `TASK_LOGON_INTERACTIVE_TOKEN` and needs none. So the start mode selects the
*backend*, and `logon` is the default: a multiplexer is per-user and per-session, and a shell
spawned from session 0 runs in the service's window station where any GUI program it launches is
invisible. The SCM backend is **not implemented yet** (it needs the credential prompt plus a
`StartServiceCtrlDispatcherW` mode in the daemon, which would be a fourth shutdown trigger
alongside the three above); `boot`/`manual` report exactly that rather than quietly registering
under LocalSystem, whose different `%TEMP%`/`%USERNAME%` would bind a socket no client could find.

The logon trigger is bound to the installing user's SAM name, not left blank — a blank one fires
on *any* user's logon.

The client side is the GUI (`contour client`) on Windows as everywhere else.
There was briefly a console client here — `runAttach`, raw VT console mode plus
a dedicated blocking stdin-reader thread, since console handles cannot park on
the socket reactor — and it was dropped along with the TUI thin client
(`d4ce9c96`); a future TUI mode plugs back in at the same seam. The console
handling is the part worth remembering if it ever returns.

Windows code is exercised by the Windows CI job (compile under -Werror plus the
runtime-gated unix-echo net test); it cannot run on the Linux development
machines.

## Diagnostics

The daemon logs through `crispy::logstore`. Categories are dotted and module-first, so
`vthost.*` maps 1:1 onto `src/vthost/`; `contour list-debug-tags` prints them all.

| Category | Default | What it records |
| --- | --- | --- |
| `vthost.daemon` | **on** | Endpoints bound, listeners started, signals, shutdown. Banner-grade only — with no `--log` and no `$LOG` these lines are the foreground user's only feedback. |
| `vthost.conn` | off | Per-connection accept, handshake and disconnect. |
| `vthost.session` | off | Hosted session spawn, resize, model refusal, exit. |
| `vthost.tmux` | off | Control-mode and imsg attach/rejection. |
| `vthost.client` | off | The attach side of the native protocol. |
| `vthost.trace.proto` | off | One line per PDU, both directions. |
| `vthost.trace.tmux` | off | Every control-mode line, both directions. |

Genuine failures — malformed frames, rejected handshakes, refused sends — go to the
always-enabled `error` category, deliberately with **no** category of their own: they must be
visible without anyone having thought to enable a filter first. `logstore::configure` therefore
keeps `error` on no matter what the filter selects.

`net` has no categories at all. It returns every failure as a `std::expected<…, NetError>` whose
`context` carries the detail (including the OpenSSL text on a TLS handshake), and the connection
that owns the transport prints it *with* its identity — so logging inside `net` would only
duplicate that line, minus the part that makes it useful.

### Selecting and directing output

```sh
contour daemon --log 'vthost.conn,vthost.session'   # a specific pair
contour daemon --log 'vthost.*'                     # everything, TRACE INCLUDED (see below)
contour daemon --log 'vthost.trace.*' --log-file /var/log/contour-daemon.log
LOG='vthost.*' contour daemon                       # the environment variable still works
```

Three behaviours worth knowing:

- **`*` is a plain prefix match**, so `vthost.*` sweeps in `vthost.trace.*` too — exactly as
  `vt.*` already sweeps in `vt.trace.sequence`. Name `vthost.trace.*` to select the trace tier
  alone, or list categories explicitly to exclude it.
- **A filter is a selection, not a union**: `logstore::configure` disables every category the
  filter does not name. `error` is deliberately exempt — asking for extra detail must never
  take failure reporting away.
- **`--log-file` appends** and never writes SGR escapes, so a daemon restarted against the same
  file does not erase why the previous run died. `--log-file -` means standard error explicitly.

`contour client` accepts both options and **passes them to a daemon it auto-spawns**. That
matters more than it looks: the auto-spawned daemon is the common case, and without this it is
the one instance nobody can configure, whose standard error lands in the client's terminal. The
whole spawn command line — the socket, these two options and `--exit-with-last-session` — is built
once in `daemonSpawnArgs`, so the POSIX `execv` argv and the Windows `CreateProcess` command line
cannot drift apart.

### Correlating a connection

Every line about one connection carries its identity — `<endpoint>#<n>`, plus `@<peer>` where
the transport knows one — so a whole connection reads as one story:

```
[vthost.conn]        native#3: accepted
[vthost.trace.proto] native#3 recv #1 ClientHello version=1 token=yes settings=yes (58 bytes)
[vthost.conn]        native#3: handshake complete (codec v1)
[vthost.trace.proto] native#3 send #0 Delta session=1 gen=3 seq=98 lines=7 (412 bytes)
[error]              native#3: malformed frame (MalformedVarint); dropping the connection
```

The endpoint name is constructor-injected into each `ConnectionAcceptor` (`control`, `native`,
`imsg`, `tmux-compat`, `native-tcp`) and travels to the connection flow as a `ConnectionId`.

### What a trace deliberately does not show

`proto::summarize` reports **sizes, never payloads**: `Input` gives a byte count, `Delta` a line
count, `SessionState` the lengths of the title and cwd. A `ClientHello`'s preshared token is
reported only as present or absent. A trace of a busy session must not become a transcript of
the user's keystrokes or screen contents, and a token mismatch — which the wire answer
deliberately makes indistinguishable from a version mismatch — must not be undone on disk.

Adding a PDU to the catalog means adding one row to `TraceTable` in `proto/PduTrace.cpp`; a
`static_assert` against `std::variant_size_v<DecodedPdu>` makes forgetting it a build break.

### Threading

Every emission point runs on the event-loop thread. Session pump threads marshal through
`EventLoop::post` before anything logs, and both shutdown hooks (the POSIX `sigwait` thread and
the Win32 console-control handler) log from inside their *posted* continuation rather than the
handler body. `logstore` has no synchronization of its own, so the daemon's sink serialises
write-and-flush under a mutex; that covers the residual cross-thread cases.
