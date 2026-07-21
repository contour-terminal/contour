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
- [ ] 3d. attach client (remote-populated `TerminalSession` seam)

## Phase 4 — tmux client

- [ ] control-mode parser (tmux.pest port + our field-level parsing), gateway state machine,
      layout ingest, capture-pane history replay, send-keys encoding

## Phase 5 — binary imsg IPC (gated; abort criteria in docs/internals/mux.md)

- [ ] rewritten-imsg framing + MSG_IDENTIFY handshake + socket discovery

## Cross-cutting

- [ ] `docs/internals/mux.md` + mkdocs nav; retire `docs/drafts/daemon-mode.md` pointer
- [ ] delete this file before the branch lands
