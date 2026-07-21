# Multiplexer & daemon mode

Contour can run as a terminal multiplexer: a Qt-free daemon process owns the
sessions and their layout, and thin clients attach over sockets. Two protocol
families are served, and the same code also speaks the client side of both:

- **tmux control mode** — external tmux-aware tooling (iTerm2-style clients,
  scripts) drives Contour's tabs and panes over the tmux 3.7b line protocol.
- **the native cells+deltas protocol** — the server emulates, the client
  renders: per-line grid deltas addressed by stable row ids (wezterm's model).

The module stack, bottom-up: `src/coro` (C++23 coroutine primitives, a
verbatim port of endo's `src/coro` at commit `178cb496`; the re-sync recipe is
in `src/coro/README.md`) → `src/net` (reactor `EventLoop`, sockets, buffered
readers, write queues) → `src/muxserver` (the daemon, both protocol servers,
and both client engines). The GUI may depend on `muxserver`, never the
reverse.

## One session, two taps

A hosted session is `{Pty, vtbackend::Terminal}` owned by the daemon's
`SessionHost`, pumped by a dedicated blocking-read thread exactly like the
GUI's `TerminalSession::mainLoop`. Everything else — sockets, protocol state,
the `vtmux::SessionModel` — is confined to one reactor thread; the pump
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

The server (`muxserver/tmux/ControlSession`) speaks the line protocol exactly
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
  reprojects every pane's PTY size through `vtmux::layoutInCells`, so
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

The wire format (`muxserver/proto/`) is wezterm's codec shape without the
dependencies: a frame is `varint taggedLength, varint serial, varint ident,
payload`, where the tagged length's low bit is a reserved compression flag
(never set, always rejected — compression can arrive later without a version
break). `serial == 0` marks unsolicited server pushes. Unknown idents decode
to `Invalid{ident}` — data, not an error — so newer peers keep talking.
A `ClientHello/ServerHello` version handshake precedes everything.

Line PDUs carry the full per-cell renditional state (grapheme cluster extras,
widths, OSC 66 scales and packed extras, hyperlink ids, colors, the 20
CellFlags bits) plus per-batch side tables: hyperlink id→URI (sent once per
connection on first reference, immune to the server-side LRU evicting the id
later) and image-covered cells. Image pixels are never inline: clients fetch
by stable image id (`FetchImage` → `ImageData`/`ImageGone`), served from
`ImagePool`'s id→weak_ptr index — eviction stays refcount-driven.

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
signal. `client/AttachClient` mirrors sessions into `RemoteScreen` — plain
data any frontend can render — and `contour attach` is a working thin client
on top of it (`client/TtyRenderer` repaints the local TTY; Ctrl-\ detaches).
Sessions survive detach; a reattach replays history. The daemon serves the
native protocol on `<control-socket>-native`.

## Socket conventions

`contour daemon`/`attach` default to `$XDG_RUNTIME_DIR/contour/<label>`,
overridable per flag and `$CONTOUR_MUX`. The socket directory hardening
mirrors tmux exactly: 0700 directory, owner check, refuse **world**-rwx
(group is permitted — tmux's `TMUX_SOCK_PERM == 7`).

## Binary imsg IPC (gated, not implemented)

Speaking tmux's *binary* client protocol (the rewritten imsg: 16-byte
`{type,len,peerid,pid}` header, `MSG_IDENTIFY_*` handshake with fds via
SCM_RIGHTS, `/tmp/tmux-<uid>/<label>` discovery) remains gated: it is the
highest-risk phase, versioned by `PROTOCOL_VERSION` (currently 8) with no
stability guarantee. Entry criteria before starting it: upstream
PROTOCOL_VERSION unchanged, struct layout identical across supported
platforms, and the required command surface not exceeding what control mode
already provides. It never blocks anything above.

## Historical note

The 2020 draft `docs/drafts/daemon-mode.md` sketched a networked-Pty design —
clients receiving a VT byte stream and parsing locally. The native protocol
deliberately inverts this (server-side emulation, cells+deltas on the wire);
the draft is retained only as a pointer here.
