# Multiplexer & daemon mode — branch roadmap

Branch-scoped deliverable tracker for `feature/mux-daemon`. Never referenced from `src/`;
deleted before this branch lands. Durable design knowledge goes to `docs/internals/mux.md`.

Goal: Contour as a terminal multiplexer — a Qt-free daemon owning sessions/layout, thin
clients over `AF_UNIX`/`AF_INET`, tmux-protocol compatibility in all three directions, and a
native cells+deltas protocol (wezterm's model). One session, two taps: raw PTY bytes feed tmux
control mode; per-line cell deltas feed the native protocol. GUI and daemon share
`vtmux::SessionModel`.

## Phase 0 — foundations

- [x] 0a. `vtmux::LayoutTree`: move `LayoutPane`/`LayoutTab`/`Layout` + serialize/realize
      helpers out of `src/contour/` (yaml emission stays behind); tests move to `vtmux_test`
- [x] 0b. `vtmux::layoutInCells` forward solver (1-cell divider, tmux `layout_check` exact)
- [x] 0c-1. `src/coro` — verbatim endo port (`endo::coro`→`coro`), + `WhenAll_test`
- [x] 0c-2. `src/net` reactor core — `EventLoop` (ex-TuiRuntime) with spawn-reap +
      `SystemPipe`-backed `post()`, `EventSource`/`PollEventSource`, `net/platform/`
- [x] 0c-3. sockets (POSIX + Win32) + `testing/InMemoryTransport`
- [x] 0c-4. `AsyncBufferedReader` (scan offset) + bounded `WriteQueue` (single writer/conn)
- [x] 0c-5. `AF_UNIX` listener + tmux-mirror socket-dir hardening

## Phase 1 — headless session host

- [x] `muxserver::SessionHost` (SessionId → {Pty, Terminal}, implements `vtmux::ModelEvents`,
      pre-mint id handshake); one reactor thread + per-session PTY threads bridged by `post()`
- [x] CLI verbs: `contour daemon`, `contour attach` (probe); socket path
      `$XDG_RUNTIME_DIR/contour/<label>`, `$CONTOUR_MUX` override

## Phase 2 — tmux control-mode server (primary use case; pin: tmux 3.7b)

- [x] line framing + `%begin/%end/%error` guards (flags bit 0 = client-originated)
- [x] `%output` octal escaping; notification emission machinery
- [x] dual-queue ordering + pause/continue flow control (budget/3, floor 32)
- [x] layout-string encoder/ingester (n-ary↔binary; tree-equality conformance against
      real `tmux select-layout` — oracle tests pass against tmux 3.7b)
- [x] data-driven command table → `SessionModel`
- [x] control-mode connection protocol (byte tap → %output; ModelEvents → notifications);
      wired into `contour daemon` and live-verified over a real Unix socket
- [x] polish: `refresh-client -C/-A/-B/-f`, resize bounds ([1, 10000], mirroring tmux's
      WINDOW_MINIMUM/MAXIMUM), `%extended-output` age field; the host reprojects PTY sizes
      through `layoutInCells` on every layout-shape change

## Phase 3 — native protocol (cells + deltas)

- [x] 3a. vtbackend retrofit (lands alone + bench/Callgrind evidence): per-line
      revision/dirty, stable row ids in ring primitives, generation resync,
      `forEachLineChangedSince`/`forEachValidLine`; `ImagePool` id→image index

  Perf evidence (for the PR description; baseline b1954fda vs 043d0014, clang-release,
  `bench-headless grid size 64 cat long binary`, 3 alternating runs, taskset-pinned):
  - Wall-clock parity: aggregate ~103.7 MB/s (base) vs ~104.5 MB/s (head); per-test
    within noise (many_lines 79.9→80.1, long_lines 262→266, binary 79.5→80.3 MB/s).
  - Callgrind total Ir: 4,787,184,907 → 4,801,584,280 = **+0.30%** (gate: <0.5%).
  - Top-20 exclusive-Ir symbol sets identical; `writeTextToSoA` and `Screen::writeText`
    Ir byte-identical (the bulk-ASCII dirty mark is one byte store per writeText call).
    Only `Grid::scrollUp` moved (+~4M Ir, 0.08% of total: assignment-op dirtying).
  - termbench-pro's `sgr` test SIGSEGVs in its own `writeNumber` (buffer underflow at
    v=0) in BOTH binaries — pre-existing third-party breakage, excluded from the runs.
- [x] 3b. leb128 PDU codec, version handshake, side tables (hyperlinks, image cells),
      `FetchImage`/`ImageData`/`ImageGone`, session-state snapshot (`src/muxserver/proto/`)
- [x] 3c. delta transport (`NativeSession`: per-connection cursors + sent-id sets, 20ms
      debounced pushes off screenUpdated, attach/resync snapshots, FetchImage service;
      daemon serves it on `<socket>-native` beside control mode)
- [x] 3d. attach client: Qt-free `client/AttachClient` (RemoteScreen mirror) +
      `TtyRenderer`; `contour attach` is a working thin client (raw TTY, input
      forwarding, Ctrl-\ detach; live-verified incl. detach/reattach survival).
      The GUI's remote-populated `TerminalSession` display seam remains follow-up
      work on top of RemoteScreen.

## Phase 4 — tmux client

- [x] `tmux/ControlModeParser`: table-driven line classifier with our field-level parsing
      (guard triples, %output octal unescape, %extended-output's ONE age field before " : ",
      layout/window/session/pause fields; unknown verbs tolerated, `%endless` != `%end`)
- [x] `tmux/TmuxGateway`: the client state machine — recovery mode until the opening guard
      (discard everything except %begin/%exit), FIFO command/response correlation,
      notification gating until the opening guard completes, `sendKeys` in 1000-char
      `send-keys -l --` batches (double-quote quoting: the dialect BOTH real tmux and our
      splitCommandLine accept — single-quote `'\''` re-quoting is NOT; loopback caught it).
      Loopback test: our gateway drives our own oracle-verified control-mode server.
- [x] live oracle run: gateway drives a real `tmux -C new-session` on its own PTY
      (`net::adoptFd` + PosixSocket's ENOTSOCK read/write fallback; PTY EIO = EOF).
      list-windows layout parses with our codec; send-keys echo confirmed via
      capture-pane polling; clean %exit detach. Self-skips without tmux.
- [x] capture-pane history replay + client-side model: `TmuxClientModel` — per-pane
      replay `vtbackend::Terminal` fed by raw %output; `capture-pane -peqJ` history
      (rows joined BETWEEN lines; live output buffered until replay landed); layout
      ingest into per-window BinaryLayout trees with pane create/resize/prune; window
      enumeration via `list-windows -F` (the server grew a minimal #{...} table).
      Follow-up: cursor position sync after replay (`#{cursor_x}/#{cursor_y}`).
- [x] GUI integration ("attach Contour to real tmux, panes mirror it"): `contour attach
      --tmux[-socket PATH]` — TmuxController spawns a real `tmux -C attach-session`
      (ControlModeSpawn promotes the oracle harness), tmux windows become tabs, panes
      split the tab, content mirrors via injected PaneSink ChannelPty feeders, input
      returns as `send-keys -H` hex (quoting-proof; oracle-verified against real tmux).
      Note: -C attach (not -CC); ratio/anchor fidelity is follow-up F3.

## GUI seams (the Qt half) — DONE

- [x] remote-populated `TerminalSession`: an ORDINARY TerminalSession over the new
      `vtpty::ChannelPty` (promoted BlockingMockPty + write/resize sinks). The
      recorded RenderBuffer-population idea was rejected during design: the display
      consumes only `session->terminal()`, so `client/ScreenMirror` re-serializes
      RemoteScreen deltas to VT and the session's own parser emulates — scrollback/
      selection/search work natively (closed-loop grid-equality tests, incl. history
      line-by-line, OSC 66 band rows, hyperlinks). Delta gained `setModes` (mirrored
      DEC input modes, CodecVersion 2). InputSerial/predictive echo stays deferred.
- [x] `contour attach --gui` — AttachController (SessionFactory + MuxLoopThread
      reactor), always-installed RoutingSessionFactory, `canCreateSession()` guards
      on every manager creation entry point; e2e-tested against an in-process daemon
      over a real AF_UNIX socket; live-verified. Enabler: SessionHost's single-slot
      screenUpdated/output handlers became a SessionStreamEvents subscriber list
      (the second-client-clobbers/disconnect-silences defect is regression-tested).

## Phase 5 — binary imsg IPC — DONE

- [x] rewritten-imsg codec (`muxserver/imsg/`): pure framing (16-byte host-order
      header, fd-mark, [16,16384] bounds), fd-claim rule (first marked header after
      the fd arrived; replaced fds close; markless-marked tolerated), MSG_COMMAND
      argv pack/unpack, table-driven MSG_IDENTIFY_* accumulation + acceptance policy.
- [x] ImsgServer: handshake → adoptFd(passed stdin/stdout) → combineHalves →
      the EXISTING ControlSession over the passed fds (options: %exit suppressed —
      the client binary prints its own; preamble guard flag 0). Lifecycle on imsg:
      drain-then-MSG_EXIT, EXITING→EXITED, MSG_VERSION on mismatch.
- [x] daemon serves `<socket>-tmux` + opt-in `--tmux-compat-socket LABEL` binding
      /tmp/tmux-<uid>/LABEL. ORACLE: the real tmux binary attaches, round-trips
      list-sessions, detaches with its own %exit, exit 0 (self-skips w/o tmux ≥ 3.6).
      Live-verified against the shipped daemon. `.lock` flock not needed (we never
      race a server start); socket exec-bit verified informational and skipped.

## Cross-cutting

- [x] `docs/internals/mux.md` (architecture, protocols, stable-id design, gated imsg
      entry criteria) + mkdocs nav line; `docs/drafts/daemon-mode.md` carries a
      supersession pointer.
- [x] simplify: the three native-protocol decode loops share `muxserver::PduPump`
      (NativeSession's handshake extracted into completeHandshake). Left as accepted:
      per-test-file waitUntil helpers (test-local idiom); RemoteScreen::viewportText vs
      TtyRenderer walk (different outputs: plain text vs VT bytes).
- [x] Windows (decision: AF_UNIX via afunix.h, not TCP): listenUnix/connectUnix live on
      the Win32 backend (parent dir created, NTFS ACLs govern access — no POSIX perm
      hardening); runDaemon serves control+native (no imsg: no SCM_RIGHTS on Windows)
      with SetConsoleCtrlHandler shutdown; runAttach = raw VT console mode + console-
      size proposal + a blocking stdin-reader thread. Pty.cpp's ConPty gate fixed
      (_MSC_VER → _WIN32). Runtime-gated unix-echo test covers the path per platform.
      VERIFICATION IS CI-ONLY (watch the Windows job after pushing).
- [ ] delete this file before the branch lands (user decision: keep until the PR).

## Follow-ups (recorded, deliberately out of v1)

- F1 LayoutState PDU: mirror the daemon's window/tab/pane layout natively (v1: one
  tab per session). F2 session-lifecycle PDUs (GUI-initiated create/split/close;
  v1: guarded off via canCreateSession). F3 tmux layout-ratio/anchor fidelity +
  GUI-initiated tmux actions (v1: split direction from the tree, active-pane anchor).
- F4 scrollback backfill PDU (history older than attach). F5 images over the native
  protocol (AttachClient still drops ImageData/ImageGone). F6 tmux %pause handling.
- F7 wrapped-flag fidelity in ScreenMirror (local rewrap of mirrored scrollback).
  F8 multi-client resize policy (last proposal wins today). F9 cursor-shape
  (DECSCUSR) propagation on the native wire. Plus: stable-id range fetch-on-demand;
  cursor sync after tmux capture replay (`#{cursor_x}/#{cursor_y}`).
- F10 (from the /simplify pass): extract the shared reactor/state/pending/binding
  scaffold of AttachController+TmuxController into a MuxController base — deferred
  because the delicate stop/teardown ordering was freshly validated and a third
  backend is what would justify the abstraction. F11: carry OSC 66 block
  origin/extent on the native wire so ScreenMirror repaints by block identity
  instead of the continuation-flag scan-up heuristic.

## Resuming on another machine

Environment:
- Build: `cmake --build --preset clang-asan`; tests: `ASAN_OPTIONS="hard_rss_limit_mb=4096"
  ctest --preset=clang-asan` (always set the RSS cap). clang-tidy is a hard gate
  (`WarningsAsErrors: '*'`): run a scoped sweep over touched files before pushing.
- The layout/protocol oracle needs tmux 3.7b installed (`/usr/bin/tmux`); oracle tests
  self-skip when absent. Reference trees via `$CONTOUR_VT_REFERENCE_SOURCES` (tmux tree
  pinned next-3.8; key files: control.c, cmd-refresh-client.c, server-client.c).
- Release perf runs: `cmake --preset clang-release -DLIBTERMINAL_BUILD_BENCH_HEADLESS=ON`.
  termbench-pro's `sgr` grid test SIGSEGVs in its own writeNumber (both baseline and
  branch) — use `grid size N cat long binary`.

Live-verification recipes (all worked on 2026-07-21):
- Control mode: `contour daemon --socket /tmp/x.sock`, then drive `/tmp/x.sock` with a
  line-based client (new-window / refresh-client -C 120x40 / -A %1:pause / list-windows).
- Native attach: `contour attach --socket /tmp/x.sock` (connects to `/tmp/x.sock-native`)
  under a real TTY; type a command, output mirrors back; Ctrl-\ detaches; reattach
  replays history. AF_UNIX paths must stay under ~100 bytes (sun_path).
- Gotcha: `pkill -f contour` matches YOUR OWN shell when the cwd/cmdline contains
  "contour" — it killed the test harness repeatedly. Use `pkill -9 -x contour`.

Known state / open ends:
- The daemon spawns the login shell; interactive shells hang at startup without the
  pump-loop `flushInput()` (commit d8edc43d) — keep that in mind when touching the pump.
- NativeSession serves ONE screenUpdated handler (last native client wins); multi-client
  fan-out needs a subscriber list on SessionHost (small, planned with the GUI seam).
- Native protocol v1 ships whole changed rows; stable-id range fetch-on-demand for
  out-of-viewport rows is an optimization left open (plan 3c).
- `%subscription-changed` (-B) is accepted but produces nothing (no format engine).
