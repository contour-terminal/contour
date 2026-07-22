# Native daemon-mode parity — implementation plan & cross-session tracker

> This file is the durable, cross-session, cross-machine progress tracker for bringing the native
> daemon clients to feature parity with the fat GUI and exposing the native protocol over TCP.
> Update its checkboxes and the Progress Log as work lands on any OS. Durable *design* knowledge
> still graduates to [`mux.md`](../mux.md); this file tracks the *process*.

## Context

Contour's daemon (`src/muxserver`) hosts sessions Qt-free and serves two protocols over "one
session, two taps": **tmux control mode** (byte-exact interop) and the **native cells+deltas
protocol** (the daemon emulates; the client renders). A prior analysis established two problems
this plan fixes:

1. **A thin client does not yet "feel the same" as the fat GUI.** The native *wire* is ahead of
   its *clients*: images, live title, clipboard, bell, notifications, cwd, cursor-shape and
   palette are dropped client-side, and the daemon's tab/pane/window **layout is never
   transmitted** (a native attach is flattened to one-tab-per-session with no split tree). The
   thin raw-TTY client (`contour attach`) is a simplified repaint, materially weaker than the GUI
   mirror.
2. **The remote use case is impossible today.** The daemon binds **AF_UNIX only**; there is no
   TCP, no auth, no encryption. The motivating scenario — a GUI client on a work PC attached to a
   daemon on a server — cannot be reached on any platform.

**Goal / outcome:** bring the **native GUI** *and* the **native thin TUI** to feature parity with
the fat GUI (VT features + tabs/panes/multi-window), and make the native protocol **exposable
over TCP/IP** (opt-in, loopback-bound by default, token-authenticated, TLS-encrypted) with a
client able to connect over TCP. Parity must hold on Linux, macOS, and Windows.

**Decisions taken (owner-confirmed):**
- **TCP security:** preshared **token auth + TLS** built this phase; listener disabled by default;
  when enabled binds `127.0.0.1` by default. **TLS backend: OpenSSL**, reached only through a
  `net::ITlsContext` dependency-injection seam.
- **Thin TUI:** unify `contour attach` onto **ScreenMirror** VT re-serialization (retire the
  simplified `TtyRenderer` repaint) — gains OSC 8, images, mouse modes, title "for free",
  bounded by the outer terminal's capabilities.
- **Clipboard:** bridge OSC 52 honoring Contour's **existing `allowClipboardRead`/write
  permissions** — no new always-on exposure.

## Baseline (verified against the tree, 2026-07-22)

Condensed native-protocol gap table (Fat GUI = target). Legend: ✅ full · ⚠️ partial · ❌ none.

| Feature | Native GUI (mirror) | Native thin (TTY) | Anchor |
|---|---|---|---|
| Images (Sixel/ReGIS/Kitty/iTerm2) | ❌ wire complete, clients drop `imageCells`; `fetchImage()` uncalled | ❌ | `NativeSession.cpp` emits; `AttachClient.cpp:171-172`, `RemoteScreen` (`AttachClient.h:55-59`) drop |
| OSC 8 hyperlinks | ✅ re-emitted | ❌ | `ScreenMirror.cpp:66-76`; `TtyRenderer.cpp:59-125` ignores |
| Live title (OSC 0/2) | ⚠️ attach/resync only | ❌ | `SessionState.title` snapshot-only (`NativeSession.cpp:254,264`) |
| OSC 52 clipboard | ❌ no PDU | ❌ | — |
| Bell / notifications | ❌ no PDU | ❌ | `SessionHost` overrides only `screenUpdated()` (`SessionHost.h:100-111`) |
| OSC 7 cwd | ❌ no field | ❌ | no `Events` signal; query `Terminal::currentWorkingDirectory()` |
| Cursor shape (DECSCUSR) | ❌ slot unpopulated | ❌ | `SessionState.cursorShape` (`Pdu.h:119`) never set |
| Palette / default fg-bg | ❌ slots unpopulated | ❌ | `SessionState` color slots never set |
| Mouse input + modes | ✅ modes mirrored | ❌ not propagated to outer TTY | `MirroredModes.h`; `ScreenMirror::syncModes` |
| Kitty-keyboard/modifyOtherKeys | ⚠️ not in mirrored set | ⚠️ | `MirroredModes.h:25-41` |
| Scrollback | ✅ real | ⚠️ received, no UI | `ScreenMirror::fullReplay`; `TtyRenderer` viewport-only |
| Selection/search/copy | ✅ native | ❌ | mirror drives a real `Terminal` |
| Layout: tabs/panes/windows | ❌ flat 1-tab-per-session | ❌ | `NativeSession.cpp:353-358` leaf-walk; no LayoutState PDU |

**Load-bearing architecture facts (these shrink the work):**
- **The daemon already owns a complete `vtmux::SessionModel`** (windows→tabs→pane trees),
  *implements* `vtmux::ModelEvents`, and already has server-side `createTab()`/`splitActivePane()`
  (`SessionHost.h:124,162,168,260`). The flattening is **only** that `NativeSession` emits per-leaf
  deltas and never subscribes to `ModelEvents`. → F1/F2 are **wire + client** work, not a new
  server model.
- **Most VT features need no vtbackend change.** `setWindowTitle`, `bell`, `notify`,
  `showDesktopNotification`, `copyToClipboard`/`getClipboard` already exist as `Terminal::Events`
  methods (`Terminal.h:282-335`); `SessionHost` simply overrides only `screenUpdated()` and drops
  the rest. Several `SessionState` slots (`cursorShape`, `cursorVisible`, `defaultForeground`,
  `defaultBackground`, `palette`) already exist on the wire and are just **unpopulated**.
- **Only cursor-shape (DECSCUSR), OSC 7 cwd, and palette/OSC 4/10/11 lack a live `Events`
  signal.** They always accompany screen output, so they can be **pull+diff'd** in
  `NativeSession::pushDelta` — no new vtbackend signal required.
- **`SessionState` is emitted snapshot-only** (`NativeSession.cpp:254`). Live state (title,
  cursor-shape, cwd, palette) must either promote into `Delta` or trigger a `SessionState`
  re-send on change.
- **Images are fully wired on the wire** already (`ImageCellEntry` + `FetchImage`/`ImageData`/
  `ImageGone`); only the client consume path is missing.
- **`MuxServer` is transport-agnostic** on `net::IListener`/`net::ISocket` (`MuxServer.h:30,40`);
  `net::listen`/`connect` (TCP, IPv4+IPv6) already exist (`Sockets.h:32-46`). TCP drops in with
  no adapter; TLS is an `ISocket` decorator.
- **PDU extension recipe** (documented `Pdu.h:9-10`): a struct + a `PduType` tag + variant member
  + one row each in `tagOf`/`encodeBody`/`decode*`/`DecodeTable` (`Pdu.cpp:70,113,246,436`). Next
  free tag is `10` (`Delta = 9`). Contract changes bump `CodecVersion` (currently 3, `Wire.h`).

## Workstreams

Sequencing follows the request: **A (parity) → B (layout) → C (TCP)**. C is isolated and may
proceed in parallel. Each PR lands with tests and stays warning-clean under `-Werror` + clang-tidy.

### WS-A — VT feature parity (native wire → both clients)

Shared enabler first, then one feature per task. Event-driven features (title/bell/notify/
clipboard) route through **new `SessionStreamEvents` signals** that `SessionHost` raises by
overriding the corresponding `Terminal::Events` methods it currently drops; state features
(cursor-shape/cwd/palette) are **pulled+diffed** in `pushDelta`.

- [x] **A0. Enabler — widen the daemon's Events tap.** `HostedSession::Events` now overrides `bell`,
      `notify`, `showDesktopNotification`, and `copyToClipboard` (title stayed on the pull path, A2),
      fanning each to new `SessionStreamEvents` callbacks (`sessionBell`/`sessionNotify`/
      `sessionCopyToClipboard`) that `NativeSession` (a stream subscriber) turns into `SessionEvent`
      PDUs. *Landed 2026-07-22 with A4/A5/A6.*
- [x] **A1a. Images — client protocol half.** `RemoteScreen` stores `imageCells` (row→column→entry)
      and a per-session `images` pixel cache; `AttachClient` fetches unknown ids on each delta
      (guarded by `requestedImages`), routes the **session-less** `ImageData`/`ImageGone` reply by
      the request **serial** (`_pendingImages`), caches on `ImageData`, and clears cells + cache on
      `ImageGone`. New `setImageHandler` fires a repaint hook. Unit-tested (RemoteScreen apply/evict/
      dropImage + a socket-level fake-server fetch/cache test). *Landed 2026-07-22; muxserver suite
      green (2606 assertions).*
- [x] **A1b. Images — render half.** `ScreenMirror` re-emits cached pixels via the **Good Image
      Protocol** (both ends are Contour, so GIP is guaranteed and its `L=` layer maps 1:1 to the
      captured `ImageLayer` — no z-index loss): upload once per id by name `muximg_<id>`
      (`o=u`, `f`=fmt+1), then place per visible image at its `(0,0)` anchor spanning the reported
      `c×r` cell box (`o=r`, `z=3` StretchToFill, no cursor move), released on drop (`o=d`). Wired
      into `apply`/`fullReplay` (after paint) and the new `setImageHandler` repaint. Closed-loop
      test drives a real GIP image through the server and asserts the mirror covers the same cells.
      *Landed 2026-07-22; muxserver suite green (113 cases / 2610 assertions).* Completes roadmap
      **F5**. Follow-up: the source alignment/resize policy is not on the native wire yet (v1 uses
      StretchToFill), and images whose anchor scrolled into history aren't re-placed until a resync.
- [x] **A2. Live title.** No Events tap needed — `screenUpdated` fires per input batch, so
      `NativeSession::pushDelta` pull+diffs `windowTitle()` against `FollowState.lastTitle` and sets
      `Delta.titleChanged`/`Delta.title` when it changed (a title-only batch now also passes the
      send gate). `RemoteScreen` adopts it; `ScreenMirror::apply` emits `OSC 0` incrementally, so the
      mirror terminal's own `setWindowTitle` drives the GUI tab title. CodecVersion → 4. Closed-loop
      test (OSC 2 with no cell change → mirror re-titles). *Landed 2026-07-22; suite green (114/2612).*
- [x] **A3. Cursor shape (DECSCUSR).** `pushDelta` pull+diffs `decscusrPs(cursorShape(),
      cursorDisplay())` (1 blink block … 6 steady bar; Rectangle→block) into `Delta.cursorShape`
      /`SessionState.cursorShape`; `ScreenMirror` re-emits `CSI Ps SP q`. CodecVersion → 5. Closed-loop
      test. **Retires roadmap F9.** Cursor **visibility** already rides `Delta.setModes` mode 25.
      *Landed 2026-07-22.* **Follow-up (A3b):** default fg/bg + indexed palette (OSC 4/10/11) — the
      `SessionState` slots exist but stay unpopulated; cells carry explicit colors, so this only
      affects default/indexed-colored cells resolving against the client's own palette.
- [x] **A4. Bell.** `SessionEvent{kind=Bell}` → `ScreenMirror::applyEvent` re-emits `BEL` into the
      mirror terminal, so the frontend's own `bell()` fires. (A single data-driven `SessionEvent` PDU
      — a new tag, no codec bump — serves A4/A5/A6; adding an event kind is a row.) *Landed 2026-07-22.*
- [x] **A5. Desktop notifications.** `notify`/`showDesktopNotification` → `SessionEvent{kind=Notify,
      a=title, b=body}` → `applyEvent` re-emits `OSC 777 notify;title;body`, so the mirror raises
      `notify()`. *Landed 2026-07-22.* Follow-up: `;` in title/body isn't escaped (OSC 777 splits on
      it); discard/replace (OSC 99 identifier) not yet carried.
- [x] **A6. OSC 52 clipboard.** `copyToClipboard` → `SessionEvent{kind=ClipboardSet, a=selection,
      b=data}` → `applyEvent` re-emits `OSC 52` (base64), so the mirror's `copyToClipboard` fires
      **under the client's own permission** (the daemon forwards unconditionally — `Terminal::
      copyToClipboard` is ungated; read stays gated by `Settings::allowClipboardRead`). *Landed
      2026-07-22.* Closed-loop tests for all three (bell/notify/clipboard) via a recording mirror.
- [x] **A7. OSC 7 cwd.** `SessionState.cwd` + `Delta.cwd`/`cwdChanged`; `pushDelta` pull+diffs
      `currentWorkingDirectory()`; `ScreenMirror` re-emits `OSC 7`, so the mirror terminal's
      `currentWorkingDirectory()` (which the GUI queries for split-in-same-dir) tracks the server.
      CodecVersion → 6. Closed-loop test. *Landed 2026-07-22.*
- [ ] **A8. Input-encoding modes beyond `MirroredModes`.** Add Kitty keyboard (`CSI>u`) and
      `modifyOtherKeys` to the mirrored set (or a negotiated-modes sub-channel) so client input
      encoding matches the server-side app's negotiation (`MirroredModes.h:25-41`).
- [ ] **A9. Unify the thin client on `ScreenMirror`.** Replace `TtyRenderer`'s simplified repaint
      with ScreenMirror VT re-serialization to the outer TTY; `syncModes` enables mouse reporting on
      the outer terminal so `pumpStdin` forwards real mouse/OSC-8/image-capable input. Optional:
      a minimal scrollback UI. Retires `TtyRenderer` as the primary path.

### WS-B — Layout: tabs / panes / multi-window (roadmap F1/F2)

- [ ] **B1. `LayoutState` PDU (F1, outbound).** New PDU carrying the window→tab→pane tree:
      per-node `{orientation, ratio}`, per-leaf `{sessionId, paneId}`, plus active-pane, zoom
      (`Tab::zoomedLeafId()`), active-tab, and tab title/color. `NativeSession` subscribes to
      `ModelEvents` via `SessionHost::subscribe` (`SessionHost.h:177`) and emits on each callback +
      in the attach snapshot. Reuse the recursive orientation/ratio capture of
      `serializePane`/`serializeTab` (`LayoutTree.cpp:142-172`) **but add the ids** the existing
      `LayoutPane` intentionally omits. The daemon already reprojects PTY sizes before fan-out
      (`fanOutAfterReproject`), so geometry is correct for free.
- [ ] **B2. Client applies `LayoutState` (AttachController).** Replace the
      `onUpdate`→`PendingSession` flattening (`AttachController.cpp:108-142`) with a layout applier
      that drives the GUI's own `SessionModel` verbs (`createTab`/`splitActivePane`/`setPaneRatio`/
      zoom/activate) to reproduce the daemon tree; keep session↔pane binding via `_bindings`. Mirror
      re-serialization (`onUpdate`) unchanged.
- [ ] **B3. Lifecycle PDUs (F2, inbound).** `CreateTab`/`SplitPane`/`ClosePane`/`MoveTab`/
      `ActivateTab`/`SetPaneRatio`/… routed to the existing `SessionHost`/`SessionModel` verbs; the
      resulting `ModelEvents` fan-out mirrors the change to every client (incl. the B1 emitter).
      Add outbound send verbs on the client next to `sendInput`/`requestResize`
      (`AttachController.cpp:216-227`). **Relax `canCreateSession()`** from "is a session pending" to
      "is the connection live" (`AttachController.cpp:186`), which lifts the four
      `TerminalSessionManager` gates (`:128,:187,:345,:1289`); **retire the `_closedSessions`
      tombstone** once a real close verb exists (`AttachController.h:147-153`).
- [ ] **B4. Multi-window onto one daemon.** Decide the window model: v1 = client opens multiple GUI
      windows, each bound to a daemon window via `LayoutState` (the daemon starts with one window;
      grow to multiple via lifecycle PDUs). Note **F8** multi-client resize policy (last-proposal-wins
      today) — pick per-client viewport vs shared-grid.
- [ ] **B5. (parallel, lower priority) tmux-path polish.** F3 ratio/anchor fidelity + GUI→tmux
      `split-window`/`kill-pane`; wire `%window-renamed`→tab title; fix Contour's own `capture-pane`
      to preserve SGR and support `-S -` scrollback; F6 client-side `%pause` reaction. Interop-only.

### WS-C — TCP + TLS + token transport (opt-in, loopback default)

- [x] **C1. TLS `ISocket` decorator in `net/`.** `net/Tls.{h,cpp}`: a `TlsSocket` wrapping any
      `ISocket`, driving OpenSSL through two memory BIOs so the handshake and records ride the same
      coroutine reactor (async, no blocking thread) — lazy handshake on first I/O, `flushOut`/`feedIn`
      pump ciphertext to/from the inner socket. OpenSSL is reached **only through `net::ITlsContext`**
      (`wrap(inner)`), so no OpenSSL type crosses `net`'s headers (linked `PRIVATE`). Factories:
      `makeTlsServerContext(certPem,keyPem)`, `makeSelfSignedServerContext()` (ephemeral RSA-2048,
      the zero-config TOFU default), `makeTlsClientContext(caPem={})` (empty ⇒ VERIFY_NONE / TOFU,
      the token authenticates). CMake `find_package(OpenSSL REQUIRED)`. Tests: `net` handshake+echo
      over a socketpair, **and** the full composition — native protocol + token **over TLS over real
      TCP** — mirrors a snapshot. *Landed 2026-07-22; net suite green (34/151), muxserver suite green
      (120/2632).* **Remaining: wire the server context into `runDaemon`'s TCP handler and the client
      context into the TCP connect path (C5).**
- [x] **C2. Daemon TCP listener.** `DaemonConfig.nativeTcp` (`NativeTcpListenerConfig`:
      host=127.0.0.1, port, token). `runDaemon` (POSIX + Win32, the latter refactored to the same
      `std::vector<MuxServer*>` shape) binds `net::listen` and serves the native protocol with
      `makeNativeHandler(loop, host, token)` — no adapter, since `MuxServer` is transport-agnostic.
      **Real-TCP end-to-end test** (ephemeral loopback port via `IListener::localPort()`, token-guarded,
      snapshot mirrors) — proving the native protocol works over TCP. *Landed 2026-07-22; suite green
      (119/2628).* **TLS wrapping is C1; CLI/config to populate `nativeTcp` is C3/C4 (Qt-side).**
- [ ] **C3. Config schema.** `NativeTcpListenerConfig { bool enabled{false}; std::string
      host{"127.0.0.1"}; uint16_t port{...}; std::string tlsCertPath; std::string tlsKeyPath;
      std::string token; }` modeled on `ImagesConfig` (`Config.h:434-439,1158`;
      `ConfigDocumentation.h:1046-1057,2369`; `Config.cpp:793,1792-1814`). **Note:** the Qt-free
      daemon builds `DaemonConfig` from CLI only (`ContourApp.cpp:561-571`) — either feed via CLI
      (lower friction) or have `daemonAction()` load `config::Config` and copy the sub-struct.
- [ ] **C4. CLI flags.** `daemon`: `--listen-tcp HOST:PORT` (+ `--tls-cert`/`--tls-key`/`--token`)
      at `ContourApp.cpp:742-761`, read in `daemonAction()`. `attach`: `--connect-tcp HOST:PORT`
      (+ `--token`, `--tls-fingerprint`) at the `attach` block, read in `attachAction()`.
- [ ] **C5. Client connect branch.** Thread a transport/endpoint (unix path OR host:port+tls+token)
      through `runAttach` (`Daemon.h:59`) and `AttachController` (replace the bare `_socketPath`,
      `AttachController.cpp:55-57`; also `Daemon.cpp:263` POSIX / `:505` Win32). Branch
      `connectUnix` vs `connect`+`TlsSocket`.
- [x] **C6. Token auth in the native handshake (core).** `ClientHello.token` (CodecVersion → 7);
      `NativeSession` gained an `expectedToken` (ctor + `makeNativeHandler` param), checked in
      `completeHandshake` right after the version check — a mismatch answers `ServerHello` and drops,
      revealing nothing (empty token accepts any: the AF_UNIX default). `AttachClient` gained a token
      ctor param it sends in the ClientHello. Tests: accept / reject / no-token-configured. *Landed
      2026-07-22.* **Remaining (C4, Qt-side):** the daemon must read the configured token (CLI/config)
      and pass it to `makeNativeHandler`; the client must read `--token` and pass it to
      `AttachController`/`runAttach` — CI-verified (Qt not buildable in the muxserver-only tree here).

## Cross-platform verification matrix

Track per workstream × OS. Windows lacks imsg/SCM_RIGHTS (irrelevant here) and hardens sockets via
NTFS ACLs, not POSIX bits; macOS resolves the socket dir under `$TMPDIR` (no `$XDG_RUNTIME_DIR`).

| | Linux | macOS | Windows |
|---|---|---|---|
| WS-A parity (GUI + thin) | ☐ | ☐ | ☐ |
| WS-B layout | ☐ | ☐ | ☐ |
| WS-C TCP+TLS (daemon + client) | ☐ | ☐ | ☐ |

Windows verification is **CI-gated** (no Windows dev box): compile under `-Werror`, run the
runtime-gated net tests, watch the Windows job after each push.

**Build note (this machine):** the configured tree is `out/build/clangcl-release`, which builds the
`muxserver`/`net`/`vtbackend` libraries and their Catch2 tests (`muxserver_test`, …) but **not the Qt
`contour` GUI**. So the WS-A/B/C work in `src/muxserver`, `src/net`, `src/vtbackend` is built+tested
here; the Qt-side pieces (`contour/mux/AttachController`, `TerminalSessionManager`, `Config`,
`ContourApp` CLI) are implemented but **CI-verified**, and are called out per task.

## Testing / verification

- **Unit:** extend `proto/Pdu_test` (new PDUs round-trip + `Invalid` forward-compat),
  `NativeSession_test` (image serve, state diff, layout emit, token gate), `AttachClient_test`
  (image cache by (session,id), layout apply, lifecycle send), `ScreenMirror_test` (image re-emit,
  incremental title, grid-equality), and a new `net` TLS handshake test (runtime-gated).
- **Loopback e2e:** the client engine drives the in-process daemon over a real socket
  (`AttachController_test` pattern) — layout create/split/close round-trips; image fetch;
  TCP+TLS+token connect/reject.
- **Live recipes:** `contour daemon --socket /tmp/x.sock`; `contour attach --gui`/`attach` →
  verify images render, tab/split mirror, live title, bell, notification, clipboard; then
  `contour daemon --listen-tcp 127.0.0.1:PORT --tls-cert … --token …` and
  `contour attach --connect-tcp 127.0.0.1:PORT --token …`. (AF_UNIX paths must stay ≲100 bytes.)
- **Perf:** WS-A/B add no vtbackend hot-path work (pull+diff of palette/cursor is per-Delta, cheap);
  if any vtbackend signal is added (A8/DECSCUSR), re-run the Callgrind gate (<0.5% Ir) per AGENT.md.
- **Gates:** `-Werror` clean, clang-tidy (`WarningsAsErrors: '*'`) scoped sweep on touched files,
  and `/simplify` before each PR; run the suite under ASan/UBSan (`ctest --preset=clang-asan`,
  `ASAN_OPTIONS=hard_rss_limit_mb=4096`), TSan for the TLS/socket threading.

## Progress log (append-only; date · machine/OS · change)

- 2026-07-22 · planning · plan authored from the parity analysis; decisions locked (TCP=token+TLS
  via OpenSSL behind a `net::ITlsContext` DI seam, thin=ScreenMirror, clipboard=existing settings).
- 2026-07-22 · Windows/clangcl-release · **A1a done** — native image fetch/cache/drop on the client
  (`AttachClient`/`RemoteScreen`); serial-correlated session routing for the session-less reply.
  3 new tests; full muxserver suite green (112 cases / 2606 assertions), build `-Werror` clean.
- 2026-07-22 · Windows/clangcl-release · **A1b done** — `ScreenMirror` re-emits images via GIP
  (upload-once-by-name + StretchToFill placement, layer-faithful); closed-loop GIP round-trip test.
  Suite green (113 cases / 2610 assertions). **WS-A1 (images) complete.**
- 2026-07-22 · Windows/clangcl-release · **A2 done** — live window title over the wire
  (`Delta.title`, pull+diff in `pushDelta`, `ScreenMirror` OSC 0). CodecVersion → 4. Suite green
  (114/2612).
- 2026-07-22 · Windows/clangcl-release · **A0 + A4/A5/A6 done** — Events tap (`HostedSession` now
  captures bell/notify/clipboard → new `SessionStreamEvents` callbacks) + a data-driven `SessionEvent`
  PDU (tag 10, kind = Bell/Notify/ClipboardSet). `ScreenMirror::applyEvent` re-emits BEL / OSC 777 /
  OSC 52 into the mirror terminal, so the frontend's own handlers + permissions apply. Recording-mirror
  closed-loop tests. Suite green (115/2619).
- 2026-07-22 · Windows/clangcl-release · **A3 done** — live cursor shape (DECSCUSR) via
  `Delta.cursorShape` (pull+diff) + `ScreenMirror` `CSI Ps SP q`. CodecVersion → 5. Retires F9. Suite
  green (116/2621). A3b (default colors/palette) deferred.
- 2026-07-22 · Windows/clangcl-release · **A7 done** — live OSC 7 cwd (`SessionState.cwd` +
  `Delta.cwd`, pull+diff, `ScreenMirror` re-emit). CodecVersion → 6. Suite green (117/2623).
  **WS-A VT features now: A1/A2/A3/A4/A5/A6/A7 done; A3b, A8, A9 remain.**
- 2026-07-22 · Windows/clangcl-release · **C6 core done** — preshared token auth in the native
  handshake (`ClientHello.token`, `NativeSession` expectedToken + `makeNativeHandler` param,
  `AttachClient` token). CodecVersion → 7. accept/reject/no-token tests. Suite green (118/2626).
  Daemon/client CLI wiring of the token is Qt-side (C4), CI-verified.
- 2026-07-22 · Windows/clangcl-release · **C2 done** — opt-in daemon TCP listener
  (`DaemonConfig.nativeTcp`, `runDaemon` POSIX+Win32 wiring via `net::listen` + transport-agnostic
  `makeNativeHandler`). Real-TCP end-to-end test (native protocol + token over loopback TCP). Suite
  green (119/2628).
- 2026-07-22 · Windows/clangcl-release · **C1 done** — async OpenSSL `TlsSocket` decorator behind
  `net::ITlsContext` (memory-BIO handshake over the reactor); self-signed / PEM / client factories.
  Tested in isolation (`net` handshake+echo) **and in composition** (native + token + **TLS over TCP**
  mirrors a snapshot). net suite 34/151, muxserver suite 120/2632. **The remote use case now works
  end-to-end, encrypted + authenticated, in the buildable layer.** Remaining for remote: wire the TLS
  contexts into `runDaemon`/`runAttach` (server buildable; client connect + Qt `AttachController` = C5),
  and the `--listen-tcp`/`--connect-tcp`/`--token`/config schema (C3/C4, Qt).

## Open decisions / risks

- **TLS backend** (C1): **decided — OpenSSL, behind a `net::ITlsContext` DI seam.** Remaining
  sub-decision: cert trust model (TOFU fingerprint pin vs configured CA) to confirm during C1.
- **Live-state transport shape** (A2/A3/A7): promote fields into every `Delta` vs a targeted
  `StateChanged` PDU / `SessionState` re-send. Recommend a small incremental state record emitted
  only on diff.
- **Multi-window/daemon window model** (B4) and **multi-client resize policy** (F8): shared grid
  (last-proposal-wins, today) vs per-client server-side viewports.
- **`CodecVersion` cadence:** batch the WS-A/WS-B additions behind one bump per landed PR;
  handshake stays exact-match, so client and daemon must be build-compatible across machines.
