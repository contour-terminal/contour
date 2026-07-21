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
- [ ] live oracle run: gateway against real `tmux -C new-session` (tmux 3.7b). Needs a
      pipe/socketpair transport into a spawned process — net has no child-process ISocket
      yet; either add `net::spawnPiped()` or dup2 a socketpair into the child. Gate the
      test on tmux availability like LayoutString_test's oracle harness (popen + private
      `-S` socket, see its `OracleServer` scope guard).
- [ ] capture-pane history replay: on attach, `capture-pane -peqJ -t %N` per pane, body
      replayed through a real `vtbackend::Terminal` (VT-with-escapes; SGR carries across
      lines; NO images — inherited tmux limitation, document in mux.md).
- [ ] client-side window/pane model: consume layoutChanged via `parseLayout` +
      `collapseToBinary` into a vtmux-shaped tree the GUI can realize (LayoutTree exists).
- [ ] GUI integration ("attach Contour to real tmux -CC, native panes mirror it"): needs
      the same remote-populated display seam as 3d's GUI half — see "GUI seams" below.

## GUI seams (deferred from 3d/4 — the Qt half)

- [ ] remote-populated `TerminalSession`: a TerminalSession variant fed by
      `client::RemoteScreen` (native attach) or a per-pane replay Terminal (tmux attach)
      instead of a local PTY+parser. RenderBuffer must be populated from RemoteScreen
      cells (colors/flags are raw wire words; `TtyRenderer.cpp` shows the decode via
      `std::bit_cast<vtbackend::Color>` / CellFlag masks). Defer InputSerial/predictive
      echo (plan decision).
- [ ] `contour attach --gui` (or a Config/session flag) wiring the above into the
      existing window/tab machinery; tmux windows/panes map onto vtmux::SessionModel.

## Phase 5 — binary imsg IPC (gated; highest risk)

- [ ] rewritten-imsg framing (16-byte header {type,len,peerid,pid} host order, len incl.
      header, top bit = fd-present, max 16384; peerid low 8 bits = PROTOCOL_VERSION 8),
      MSG_IDENTIFY_* handshake with STDIN/STDOUT fds via SCM_RIGHTS, three-way shutdown,
      `/tmp/tmux-<uid>/<label>` discovery + `.lock` flock dance.
      ABORT criteria: upstream PROTOCOL_VERSION bump; struct layout differs across
      supported platforms; required command surface exceeds Phase 2's. Never blocks 0-4.

## Cross-cutting

- [ ] `docs/internals/mux.md`: architecture (two taps, threading model, stable-id delta
      design incl. the floor/generation rules, wire format v1), tmux 3.7b pin rationale,
      endo provenance (commit 178cb496, re-sync recipe in src/coro/README.md), inherited
      limitations (no images in pre-attach tmux history; capture-pane text+SGR only),
      Phase 5 abort criteria. Add one nav line under `Internals:` in mkdocs.yml.
- [ ] retire `docs/drafts/daemon-mode.md` with a pointer to mux.md (its networked-Pty
      client design is superseded by cells+deltas).
- [ ] `/simplify` pass over `src/muxserver` (duplication check: the three binary decode
      loops in NativeSession/AttachClient/TmuxGateway::run share a shape; the two
      `waitUntil` test helpers; RemoteScreen::viewportText vs TtyRenderer cell walk).
- [ ] Windows: `runDaemon`/`runAttach` are stubs; net's Win32 backend compiles but
      `listenUnix/connectUnix` return Unsupported — decide AF_UNIX-on-Windows vs TCP.
- [ ] delete this file before the branch lands.

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
