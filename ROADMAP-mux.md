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

- [ ] `muxserver::SessionHost` (SessionId → {Pty, Terminal}, implements `vtmux::ModelEvents`,
      pre-mint id handshake); one reactor thread + per-session PTY threads bridged by `post()`
- [ ] CLI verbs: `contour daemon`, `contour attach`; socket path
      `$XDG_RUNTIME_DIR/contour/<label>`, `$CONTOUR_MUX` override

## Phase 2 — tmux control-mode server (primary use case; pin: tmux 3.7b)

- [ ] line framing + `%begin/%end/%error` guards (flags bit 0 = client-originated)
- [ ] notifications incl. `%output` octal escaping, `%extended-output`, `%layout-change`
- [ ] dual-queue ordering + pause/continue flow control (budget/3, floor 32)
- [ ] layout-string encoder/ingester (n-ary↔binary; tree-equality conformance against
      real `tmux select-layout`)
- [ ] data-driven command table → `SessionModel`

## Phase 3 — native protocol (cells + deltas)

- [ ] 3a. vtbackend retrofit (lands alone + bench/Callgrind evidence): per-line
      revision/dirty, stable row ids in ring primitives, generation resync,
      `forEachLineChangedSince`/`forEachValidLine`; `ImagePool` id→image index
- [ ] 3b. leb128 PDU codec, version handshake, side tables (hyperlinks, image cells),
      `FetchImage`/`ImageData`/`ImageGone`, session-state snapshot
- [ ] 3c. delta transport (per-connection cursors, adaptive polling)
- [ ] 3d. attach client (remote-populated `TerminalSession` seam)

## Phase 4 — tmux client

- [ ] control-mode parser (tmux.pest port + our field-level parsing), gateway state machine,
      layout ingest, capture-pane history replay, send-keys encoding

## Phase 5 — binary imsg IPC (gated; abort criteria in docs/internals/mux.md)

- [ ] rewritten-imsg framing + MSG_IDENTIFY handshake + socket discovery

## Cross-cutting

- [ ] `docs/internals/mux.md` + mkdocs nav; retire `docs/drafts/daemon-mode.md` pointer
- [ ] delete this file before the branch lands
