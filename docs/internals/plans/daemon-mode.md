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
| Kitty-keyboard/modifyOtherKeys | ✅ kitty flags mirrored (A8); modifyOtherKeys deferred (wire-form clash) | ✅ | `kittyKeyboardFlags` field; `MirroredModes.h` note |
| Scrollback | ✅ real | ⚠️ received, no UI | `ScreenMirror::fullReplay`; `TtyRenderer` viewport-only |
| Selection/search/copy | ✅ native | ❌ | mirror drives a real `Terminal` |
| Layout: tabs/panes/windows | ❌ flat 1-tab-per-session | ❌ | `NativeSession.cpp:353-358` leaf-walk; no LayoutState PDU |
| Multi-page (status-line displays) | ❌ only `currentScreen()` synced | ❌ | `NativeSession.cpp:296` serializes the main grid; the host-writable/indicator status-line `Screen`s are missed |

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
      *Landed 2026-07-22.*
- [x] **A3b. Default colors (OSC 10/11).** `Delta.defaultForeground/Background` + `colorsChanged`
      (pull+diff of `colorPalette().defaultForeground/Background`), populated on the `SessionState`
      snapshot; `ScreenMirror` re-emits `OSC 10/11` (`rgb:RR/GG/BB`). CodecVersion → 8. Closed-loop
      test. *Landed 2026-07-22; suite green (121/2635).* **Follow-up:** the indexed **palette** (OSC 4,
      256 entries) is still not synced — cells carry explicit colors, so this only affects
      *indexed*-colored cells resolving against the client's own palette (deferred: 256 seqs/replay).
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
- [x] **A8. Input-encoding modes beyond `MirroredModes` — Kitty keyboard done; modifyOtherKeys
      deferred with cause.** The Kitty keyboard protocol (CSI u) is not a DEC private mode, so it can't
      join `MirroredModes` (a `DECMode` table). It rides its own live-state field instead:
      `SessionState`/`Delta` carry `kittyKeyboardFlags` (pull+diff of `Terminal::keyboardProtocol()
      .flags().value()` in `pushDelta`, the same shape as title/cwd/colors); `ScreenMirror` re-emits
      `CSI = flags ; 1 u` (the Kitty "set exactly", mode 1) — into the mirror `Terminal` (GUI) or the
      outer terminal (thin), so the client encodes keys the way the app negotiated. CodecVersion → 11.
      Tests: PDU round-trip carries the field; `ScreenMirror_test` "Kitty keyboard flags mirror so the
      client encodes keys alike" (app pushes `CSI > 5 u` → the mirror's `keyboardProtocol().flags()`
      becomes 5, and a later change to 13 propagates). **modifyOtherKeys is intentionally NOT mirrored:**
      vtbackend parses it as `CSI > n m` (single param = level), which collides with xterm's
      `CSI > 4 ; n m` form, so re-emitting it would misconfigure a real outer terminal in the thin
      client — noted in `MirroredModes.h`. Revisit only if a shared wire-form is settled. *Landed
      2026-07-22; muxserver suite green (128/2685).*
- [x] **A9. Unify the thin client on `ScreenMirror`.** `attachFlow` (POSIX + Win32) now drives the
      OUTER terminal with `ScreenMirror::apply`/`applyImage`/`applyEvent` (the same re-serialization
      the GUI feeds its mirror Terminal) instead of `TtyRenderer::renderViewport` — so the raw-TTY
      `contour attach` gains OSC 8 hyperlinks, images (GIP; bounded by the outer terminal),
      mouse-mode propagation (via `syncModes` → outer terminal reports → `pumpStdin` forwards),
      live title, default colors and cursor shape. `TtyRenderer` is retired as the primary path
      (files/tests kept). *Landed 2026-07-22.* Build clean; the `runAttach` flow itself stays
      CI/manually verified (real stdin/stdout/TTY, not headless), but its `ScreenMirror` core is
      fully unit-tested. Follow-up: a scrollback UI; ensure the outer Contour has GIP enabled for
      images.
- [x] **A10. Full multi-page support (status-line displays + DEC pages). COMPLETE (2026-07-22).**
      *(Added 2026-07-22 — the native protocol modelled only ONE grid per session; `ScreenType`
      predates Contour's multi-page support.)* All scopes landed: **Scope 2 (status-display state)**,
      **Scope 1 (host-writable status CONTENT)**, **A10.3 (DEC saved/pushed status-display stack —
      covered by construction)**, **A10.4 (DEC pages 1–14 + decoupled display)** — see the per-scope
      notes below and the progress log.
      **Scope 2 (state):** `SessionState`/`Delta` now carry `statusDisplayType` +
      `activeStatusDisplay` (pull+diff of `Terminal::statusDisplayType()`/`activeStatusDisplay()`),
      `ScreenMirror` re-emits DECSSDT (`CSI Ps $ ~`) + DECSASD (`CSI Ps $ }`); CodecVersion → 9;
      closed-loop test (DECSSDT 1 → the mirror shows the indicator status line, rendered locally from
      its own state — matching the mux philosophy). *Landed 2026-07-22; suite green (123/2654).*
      **Scope 1 (host-writable CONTENT) landed** — see the progress log (`Delta.statusLines`,
      CodecVersion → 10). Original scope:
      A session has more renderable **pages** than primary/alternate: `NativeSession` serializes only
      `terminal->currentScreen().grid()` (`NativeSession.cpp:296`), but Contour also has a
      **host-writable status line** (`_hostWritableStatusLineScreen` — an app writes it after DECSASD
      `CSI 1 $ }`) and an **indicator status line** (`_indicatorStatusScreen`), each a *separate*
      `Screen`/grid, selected by `StatusDisplayType` {None,Indicator,HostWritable} + `ActiveStatusDisplay`
      {Main,StatusLine,IndicatorStatusLine} + `StatusDisplayPosition` {Top,Bottom} (DECSSDT `$~` /
      DECSASD `$}`). Today those pages are dropped, so a thin/GUI client shows no/stale status line.
      **Scope (buildable/testable in muxserver + vtbackend, additive — `Terminal::pageSize` already
      excludes the status line, so main-grid deltas are unaffected):**
      1. Address deltas **per page**: add `Delta.page` (0 = main, 1 = host-writable status line) and a
         `FollowState` cursor per page; the server runs the same stable-id delta over
         `hostWritableStatusLineDisplay().grid()`; the client keeps a `RemoteScreen` per (session, page).
      2. Carry the status-display **state** — `StatusDisplayType`, `StatusDisplayPosition`,
         `ActiveStatusDisplay` — in `SessionState` (+ live via `Delta`, pull+diff like the colors);
         `ScreenMirror` re-emits DECSSDT/DECSASD and paints the status page in the right position, so
         the mirror terminal reproduces it (GUI) / the outer terminal shows it (TUI).
      3. The **indicator** status line is Contour-rendered from state (VTType/mode/cwd/…). Decide:
         render it locally on the client from already-synced state (keeps it client-styled, matches the
         mux philosophy) vs. ship its cells. Recommend local render → only the *host-writable* page
         needs cell sync.
      **A10.3 — DEC saved/pushed status-display stack (`savedStatusDisplayType`/`pushStatusDisplay`).
      DONE by construction (2026-07-22).** `pushStatusDisplay`/`popStatusDisplay` (driven by KAM,
      `CSI 2 h`/`l`) both mutate `_statusDisplayType` — the exact value scope 2's pull+diff already
      syncs — so the client mirrors the EFFECTIVE type through any push/pop; the save/restore stack
      stays server-side (same philosophy as the DEC pages). Verified by `ScreenMirror_test` "a pushed
      then popped status display round-trips through the mirror". Doing so **exposed and fixed a real
      bug**: `ScreenMirror::fullReplay` only emitted DECSSDT/DECSASD when non-None, so a pop-to-None
      (which resizes the main grid → forces a snapshot → fullReplay) never reset the mirror's status
      line; fullReplay now asserts the status-display state unconditionally (DECSSDT 0 / DECSASD 0 are
      no-ops when already None/Main).
      **A10.4 — DEC multi-page (`_pages`/`_cursorPage`/`_displayedPage`). DONE (2026-07-22).**
      `Terminal` holds `MaxPageCount = 16` pages (page 0 primary, 1–14 DEC pages, 15 alternate), with
      `_cursorPage` (where VT output goes) vs `_displayedPage` (what the user sees; they diverge only
      when DECPCCM page-cursor coupling is OFF), navigated by NP/PP/PPA. The daemon used to serialize
      `currentScreen()` = the **cursor page**, keyed only on the binary `screenType`. Fixed in
      `NativeSession::pushDelta` to mirror **exactly what the fat GUI paints**:
      • serialize `pageAt(displayedPageIndex())` — the **displayed** page — for both the grid and the
        cursor (also fixes a latent bug where an active status display made `currentScreen()` the
        status screen);
      • **force a resync on the displayed-page IDENTITY** (`FollowState.lastDisplayedPage`, a
        `PageIndex`), not on primary-vs-alt — this is what makes DEC-page↔DEC-page switches (all of
        which share `screenType==Alternate`) mirror, since each page is a distinct grid with its own
        colliding generation;
      • **withhold DECTCEM (mode 25)** while `cursorPageIndex() != displayedPageIndex()`, matching the
        GUI's `effectiveCursorPosition` hiding of an off-screen cursor.
      No wire/codec change — the mirror's existing `screenType` (0=primary / 1=alt-like) drives its
      `?1049` toggle and a snapshot-forced `fullReplay` repaints whichever page is displayed (the
      daemon re-sends the page's whole grid, so the mirror needs no per-page memory). Tests:
      `ScreenMirror_test` "DEC pages beyond primary/alternate mirror faithfully" (NP→page 1 content
      shows on the mirror's alt buffer, equals the server's page 1, page 0 restores on PP) and "a
      decoupled cursor page hides the mirror's cursor" (DECPCCM off + NP → mirror hides the cursor and
      keeps showing the displayed page). muxserver suite green (127/2680).

### WS-B — Layout: tabs / panes / multi-window (roadmap F1/F2)

- [x] **B1. `LayoutState` PDU (F1, outbound).** New tag-11 PDU (forward-compatible, no codec bump):
      `LayoutState{window, activeTab, tabs[]}` where each `WireTab` carries `{tabId, activePane,
      zoomedPane, title, color, root}` and `WirePane` is a recursive `{paneId, split, session,
      ratio×10000, children[]}` (depth-bounded decoder). `NativeSession` serializes the daemon's
      `vtmux::SessionModel` (`serializeLayout`/`serializePaneTree`), pushes it **leading the attach
      snapshot** (so the client builds tabs before content streams in) and **on every model change**
      via a `LayoutObserver` (a `vtmux::ModelEvents` subscribed through a new `ScopedModelSubscription`
      in `serveNativeClient`). `AttachClient` grew `setLayoutHandler`. Tests: PDU round-trip + a
      split-pane end-to-end (attach → `LayoutState` shows the vertical split, 60/40, two distinct-session
      leaves). *Landed 2026-07-22; muxserver suite green (122/2652).* **B2 (the GUI consuming it in
      `AttachController` → `SessionModel`) is the Qt follow-up.**
- [x] **B2. Client applies `LayoutState` — DONE, verified end-to-end (2026-07-22).** The GUI now
      reproduces the daemon's full tab/split tree instead of flattening one tab per session:
      1. **Capture:** `AttachController` subscribes the daemon's `LayoutState` (`onLayout` → thread-safe
         `layout()`/`wireLayout()` + `layoutChanged()` signal).
      2. **Reconstruction — reuses the SHARED realizer, not a bespoke stepper.** `muxserver/client/
         LayoutReconstruction`: `wireToLayout(LayoutState)` converts the wire tree to a `vtmux::Layout`
         (== `config::Layout`) plus a leaf→remote-session map, so it feeds the same `vtmux::
         realizeLayoutTab` the daemon and config-layout loader use. Headless-tested against a real
         `vtmux::SessionModel`.
      3. **Binding seam:** `TerminalSessionManager::applyLayoutToWindow(..., beforeLeafSeed)` calls the
         hook just before each pane's backing session is created; `contour/mux/RemoteLayout`
         `applyRemoteLayout` supplies a seeder that binds the pane to `leafSession.at(&leaf)` via
         `AttachController::setNextBindSession` (createPty binds there instead of the FIFO queue).
         `canCreateSession()` is relaxed during realization (**also the B3-Qt relaxation**).
      4. **Wiring:** `ContourGuiApp` connects `layoutChanged` → `applyRemoteLayout` (once a window +
         layout exist), replacing the `remoteSessionDiscovered`→`adoptStartupSessions` flattening
         (now removed).
      **Verified on Windows** end-to-end: the `AttachController_test` daemon fixture is a real
      TLS-over-loopback-TCP daemon (two independent reactors); a test drives a split daemon layout and
      asserts it realizes as a 2-pane vertical split in the GUI's own `SessionModel`. (Active-pane/zoom
      restoration to `WireTab.activePane` is follow-up polish.) **Bonus:** switching the fixture to real
      TCP exposed and fixed a two-reactor TLS handshake deadlock — see the progress log.
- [x] **B3. Lifecycle PDUs (F2) — COMPLETE (2026-07-23), both directions, verified.** GUI authoring
      of tabs, splits, and closes all round-trip through the daemon and reconcile back; see the B3-Qt
      note at the end of this bullet and the progress log. Server-receive half + wire below.
      Three tag-12/13/14 PDUs (forward-compatible, no codec bump): `CreateTab{}`, `SplitPane{session,
      orientation, ratio×10000}` (targets the pane by session — the daemon activates it, then splits),
      `ClosePane{session}`. `NativeSession::handlePdu` routes them to the
      existing `SessionHost` verbs (`createTab`/`splitActivePane`/`handleSessionExit`); the resulting
      `ModelEvents` fan out through **every** client's `LayoutObserver`, so the change mirrors back to
      all attached clients (incl. the author) as a fresh `LayoutState`. `AttachClient` grew the
      outbound send verbs `createTab()`/`splitPane()`/`closePane()`. Tests: PDU round-trip + an
      end-to-end where the client authors a tab and the daemon honors it (subscribed observer re-pushes
      a two-tab `LayoutState`; the daemon's `SessionModel` really grew). *Landed 2026-07-22; muxserver
      suite green (125/2672).* **GUI authoring — tab creation DONE (2026-07-22):** `SessionFactory`
      gained `requestRemoteTab()` (default false = local); `AttachController` overrides it to
      `requestCreateTab()` → `AttachClient::createTab()`; `RoutingSessionFactory` forwards it;
      `TerminalSessionManager::createNewTab` consults it and skips local creation. So a GUI "new tab"
      is authored on the daemon, whose layout re-push the incremental reconciler realizes locally.
      `canCreateSession()` **relaxed** (done with B2). **GUI authoring — ALL of tab/split/close DONE
      (2026-07-22..23), verified over a two-reactor TLS attach:** `SessionFactory` gained
      `requestRemoteTab`/`requestRemoteSplit` (forwarded by `RoutingSessionFactory`, consulted by
      `TerminalSessionManager::createNewTab`/`splitActivePane`); `AttachController` routes them to
      `AttachClient::createTab`/`splitPane`, and `unbind` (user pane close) authors `closePane`. The
      reconciler `applyRemoteLayout` is fully incremental now: whole new tabs, new splits WITHIN a tab
      (via a remote-session→`TerminalSession` map, `sessionForPty`), and a SUBTRACTIVE pass that closes
      local panes whose session left the layout — so authoring here or on another client round-trips.
      No `ActivatePane` PDU was needed: `SplitPane` carries the target session and the daemon activates
      it. `_closedSessions` is **kept** (still guards the local-close→daemon-drop race), not retired.
      `MoveTab`/`ActivateTab`/`SetPaneRatio` are additive tags (no codec bump) to add as the GUI needs.
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
      (119/2628).* **TLS wrapping now applied** (below); CLI/config to populate `nativeTcp` is C3/C4.
      **`runDaemon` (POSIX + Win32) now wraps each accepted TCP socket in server-side TLS**
      (`makeNativeTcpTls`: configured PEM cert/key, else an ephemeral self-signed cert) before the
      native handler — so the TCP transport is always encrypted. `NativeTcpListenerConfig` grew
      `tlsCertPath`/`tlsKeyPath`. *Landed 2026-07-22.*
- [x] **C3. Config (via CLI). DONE (2026-07-22).** Took the lower-friction path the note flagged:
      the Qt-free daemon builds `DaemonConfig` from the CLI, so `daemonAction()` populates
      `config.nativeTcp` (`NativeTcpListenerConfig`) directly from the flags below — no `config::Config`
      round-trip needed. (A YAML schema can graft on later if persisting the listener is wanted.)
- [x] **C4. CLI flags. DONE (2026-07-22).** `daemon`: `--listen-tcp HOST:PORT` (+ `--token`,
      `--tls-cert`, `--tls-key`), parsed in `daemonAction()` via `muxserver::parseHostPort` into
      `config.nativeTcp`. `attach`: `--connect-tcp HOST:PORT` (+ `--token`, `--tls-ca`), read in
      `attachAction()` (thin) and `ContourGuiApp::attachAction()` (GUI). `--tls-ca` pins the daemon's
      cert as the trust anchor; omitted ⇒ TOFU. `parseHostPort` handles `HOST:PORT` and `[v6]:PORT`
      with a unit test.
- [x] **C5. Client connect branch. DONE (2026-07-22).** Introduced `muxserver::AttachEndpoint`
      (a `std::variant<UnixEndpoint, TcpEndpoint>`) and a shared `connectAttach(loop, endpoint)`
      coroutine that branches `connectUnix` (native socket resolved beside the control path) vs
      `net::connect` + `makeTlsClientContext(caPem)->wrap` (TLS). `runAttach` (POSIX + Win32),
      `attachFlow`/`attachFlowWin32`, AND the GUI `AttachController` (its `_socketPath` replaced by an
      `AttachEndpoint`) all route through it, so the thin TTY client and the GUI client reach a TCP+TLS
      daemon identically. Token flows to the `AttachClient` via `endpointToken`. Tests: `parseHostPort`
      unit; a full **native-protocol-over-TLS-over-TCP mirror using a generated dev cert the client
      VERIFIES** (`AttachClient_test`); `AttachController_test` adjusted to the control-path convention.
      All suites green (net 35/160, muxserver 131/2711, contour_gui 635 cases/9765).
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
| WS-A parity (GUI + thin) | ☐ | ☐ | ☑ (muxserver+contour_gui suites green) |
| WS-B layout | ☐ | ☐ | ☑ B1/B3 wire + B2 GUI apply (split tab e2e over TCP); B4/B5 pending |
| WS-C TCP+TLS (daemon + client) | ☐ | ☐ | ☑ (net+muxserver+contour e2e incl. two-reactor TLS) |

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
- 2026-07-22 · Windows/clangcl-release · **A3b done** — live default fg/bg (OSC 10/11) over
  `Delta.defaultForeground/Background`; `ScreenMirror` re-emits. CodecVersion → 8. Suite green
  (121/2635). Indexed-palette (OSC 4) sync deferred.
- 2026-07-22 · Windows/clangcl-release · **A9 done** — thin `contour attach` unified on `ScreenMirror`
  (POSIX + Win32 `attachFlow`), so the raw-TTY client gains OSC 8 / images / mouse-modes / title /
  colors / cursor; `TtyRenderer` retired as primary path. Build clean; suite green (121/2635).
  **WS-A is now complete except A8** (kitty-keyboard/modifyOtherKeys mirroring — niche, needs a new
  vtbackend getter).
- 2026-07-22 · Windows/clangcl-release · **B1 done** — `LayoutState` PDU (recursive tab/pane tree,
  tag 11, no codec bump) + `NativeSession` serialize/emit (snapshot-leading + live via a
  `LayoutObserver`/`ScopedModelSubscription`) + `AttachClient::setLayoutHandler`. Split-pane
  end-to-end test. muxserver suite green (122/2652).
- 2026-07-22 · Windows/clangcl-release · **A10 scope-2 (status-display state) done** — multi-page
  support beyond primary/alt: `SessionState`/`Delta` carry `statusDisplayType`/`activeStatusDisplay`
  (pull+diff); `ScreenMirror` re-emits DECSSDT/DECSASD. CodecVersion → 9. Closed-loop test (indicator
  status line shows on the mirror, rendered locally). Suite green (123/2654).
- 2026-07-22 · Windows/clangcl-release · **A10 scope-1 (host-writable status-line CONTENT) done** —
  `Delta.statusLines` carries the host-writable status page's (tiny) grid whole-on-change;
  `ScreenMirror` paints it (DECSASD to the status line → cells → back). **Plus the daemon-side
  enablement:** `HostedSession::Events` now honors `requestShowHostWritableStatusLine()` (DECSSDT 2
  only *requests* the status line — the frontend decides; the GUI does the same) by calling
  `setStatusDisplay(HostWritable)`. CodecVersion → 10. Closed-loop test (app writes STATUSBAR to its
  status line → the mirror's status page shows it). Suite green (124/2658). **Remaining A10: multiple
  DEC pages (`_pages`/`_cursorPage`) — `ScreenType` collapses pages 1+ to "Alternate" and the daemon
  serializes only `currentScreen()`; investigating whether Contour uses > 2 pages.** Remaining
  overall: A8, A10-decpages, B2/B3/B4 (Qt), C3/C4/C5 (Qt), B5.
- 2026-07-22 · Windows/clangcl-release · **B3 server-receive half done** — layout-authoring PDUs
  `CreateTab`/`SplitPane`/`ClosePane` (tags 12/13/14, no codec bump) routed in
  `NativeSession::handlePdu` to `SessionHost::createTab`/`splitActivePane`/`handleSessionExit`; the
  model change fans out to every client's `LayoutObserver` as a fresh `LayoutState`. `AttachClient`
  grew `createTab()`/`splitPane()`/`closePane()`. PDU round-trip + end-to-end test (client authors a
  tab → subscribed daemon re-pushes a two-tab layout and its `SessionModel` really grew). Suite green
  (125/2672). **Remaining B3 (Qt, CI-verified):** GUI calls the verbs from `AttachController`, relax
  `canCreateSession()`, retire the `_closedSessions` tombstone. Remaining overall: A8, A10-decpages,
  B2/B4 (Qt), B3-Qt, C3/C4/C5 (Qt), B5.
- 2026-07-22 · Windows/clangcl-release · **A10.4 (DEC multi-page) done** — the daemon now mirrors the
  **displayed** page (`pageAt(displayedPageIndex())`) rather than the cursor page, forces a resync on
  the displayed-page IDENTITY (`FollowState.lastDisplayedPage`, a `PageIndex`, replacing the binary
  `lastScreenType` gate that collapsed DEC pages 1–14 to "Alternate"), and withholds DECTCEM while the
  cursor page ≠ the displayed page — reproducing the fat GUI's page selection and off-screen-cursor
  hiding. No wire/codec change (the mirror's existing `?1049` alt-toggle + snapshot-forced fullReplay
  repaints whichever page is shown). Also fixes a latent bug: an active status display no longer leaks
  the status screen into the main-grid delta. Two `ScreenMirror_test` cases (DEC-page content mirrors
  and restores across NP/PP; a decoupled cursor hides on the mirror). Suite green (127/2680).
  **WS-A multi-page (A10) is now complete except the niche DEC saved/pushed status-display stack
  (A10.3).** Remaining overall: A8, A10.3 (status-display save/restore stack), B2/B4 (Qt), B3-Qt,
  C3/C4/C5 (Qt), B5.
- 2026-07-22 · Windows/clangcl-release · **A8 (Kitty keyboard flags) done** — the Kitty keyboard
  protocol flags now ride `SessionState`/`Delta` (`kittyKeyboardFlags`, pull+diff of
  `Terminal::keyboardProtocol().flags()`); `ScreenMirror` re-emits `CSI = flags ; 1 u` so the mirror
  `Terminal` (GUI) / outer terminal (thin) encodes keys the way the app negotiated. CodecVersion → 11.
  PDU round-trip + a `ScreenMirror_test` case. **modifyOtherKeys deferred with cause:** vtbackend's
  `CSI > n m` form clashes with xterm's `CSI > 4 ; n m`, so re-emitting it would misconfigure a real
  outer terminal (noted in `MirroredModes.h`). Suite green (128/2685). Remaining overall: A10.3,
  B2/B4 (Qt), B3-Qt, C3/C4/C5 (Qt), B5.
- 2026-07-22 · Windows/clangcl-release · **A10.3 (DEC saved/pushed status-display stack) done by
  construction — WS-A now 100% complete.** `pushStatusDisplay`/`popStatusDisplay` (KAM `CSI 2 h`/`l`)
  mutate the effective `statusDisplayType()`, which scope 2's pull+diff already syncs, so the client
  mirrors any push/pop with no extra wire — the stack stays server-side (same philosophy as the DEC
  pages). Verifying it **exposed and fixed a real bug**: `ScreenMirror::fullReplay` only emitted
  DECSSDT/DECSASD when non-None, so a pop-to-None (which resizes the main grid → forces a snapshot →
  fullReplay) left the mirror's status line stuck; fullReplay now asserts the status-display state
  unconditionally. New `ScreenMirror_test` push/pop round-trip case. Suite green (129/2689). **WS-A
  (VT feature parity, native wire → both clients) is fully complete: A1–A10 all landed.** Remaining
  overall: B2/B4 (Qt), B3-Qt, C3/C4/C5 (Qt), B5 — the GUI/CLI-config items that build only in a full
  Qt tree (CI-verified), plus the interop-only tmux polish.
- 2026-07-22 · Windows/clangcl-release · **The Qt GUI DOES build here** (Qt6 6.11.1 present;
  `contour.exe` + `contour_gui_test.exe` link and `contour_gui_test` runs green — 635 cases / 9765
  assertions, 2 skipped). The earlier "Qt items are CI-only" assumption was wrong, so C3/C4/C5 and
  the GUI attach path are now buildable AND verifiable on this box (offscreen Qt platform;
  `Qt6Test.dll` must be copied beside the test binary — a one-time deploy gap, not a code fault).
- 2026-07-22 · Windows/clangcl-release · **WS-C COMPLETE — C3/C4/C5 done; the remote use case is
  reachable from the CLI end-to-end.** `contour daemon --listen-tcp HOST:PORT [--token …] [--tls-cert
  … --tls-key …]` opens the opt-in, always-TLS, token-authenticated TCP listener (self-signed when no
  cert/key); `contour attach --connect-tcp HOST:PORT [--token …] [--tls-ca …]` (thin) and
  `contour attach --gui --connect-tcp …` (GUI) dial it. Shared plumbing: `muxserver::AttachEndpoint`
  = `variant<UnixEndpoint, TcpEndpoint>`, `parseHostPort`, and one `connectAttach` coroutine that both
  `runAttach`/`attachFlow(Win32)` and the GUI `AttachController` route through (its `_socketPath`
  became an `AttachEndpoint`). Also delivered the operator's ask: `net::generateSelfSignedCertificate`
  (library-only dev certs, cross-platform) with a client-VERIFIED (pinned) TLS e2e test at both the
  `net` and `muxserver` levels. Suites green (net 35/160, muxserver 131/2711, contour_gui 635/9765),
  `contour.exe` links clean. **Remaining overall: B2/B4 (GUI layout apply + multi-window), B3-Qt (GUI
  calls the lifecycle verbs), B5 (interop-only tmux polish).**
- 2026-07-22 · Windows/clangcl-release · **B2 layout capture + reconstruction PLANNER done.**
  `AttachController` now captures the daemon `LayoutState` (thread-safe `layout()` + `layoutChanged()`
  signal), and — the hard part — `muxserver/client/LayoutReconstruction.{h,cpp}` turns a `LayoutState`
  into an ordered `NewTab`/`Split`/`Activate` plan (`planReconstruction`). The flattening algorithm is
  **verified headless** by replaying the plan against a real `vtmux::SessionModel` (controlled session
  allocator) and asserting the rebuilt tree matches — single pane, single split, a nested tree
  (re-activation path), multi-tab, empty. muxserver suite 136/2763. **Remaining B2 (Linux-CI):** the
  thin GUI executor that runs the plan against `TerminalSessionManager` and binds each reconstructed
  pane to its remote session (the `MockPtySessionFactory` harness can headless-test the tree-building;
  the remote-session binding needs the AF_UNIX daemon fixture). Plus B3-Qt, B4, B5.
- 2026-07-22 · Windows/clangcl-release · **B2 reconstruction reworked to reuse the shared realizer,
  and every reusable primitive verified.** Replaced the bespoke stepper with `wireToLayout` →
  `vtmux::realizeLayoutTab` (DRY: one tree-reconstruction path, shared with the daemon and the
  config-layout loader), headless-tested against a real `SessionModel`. Proved the daemon-layout → GUI
  realization through `TerminalSessionManager::applyLayoutToWindow` (`MockPtySessionFactory` structure
  test), and added the per-leaf binding seam `applyLayoutToWindow(..., beforeLeafSeed)` (test asserts
  every leaf resolves to its remote session). muxserver 136/2769, contour_gui 636/9777. **Remaining B2
  is one cohesive AttachController+ContourGuiApp integration** (bind-on-seed, relax `canCreateSession`,
  wire the executor) that alters the working attach path — runtime-verified on the AF_UNIX Linux
  fixture, per AGENT.md's "run the tests" rule. Plus B3-Qt, B4, B5.
- 2026-07-22 · Windows/clangcl-release · **B2 COMPLETE, verified end-to-end — and a two-reactor TLS
  bug fixed along the way.** Landed the AttachController+ContourGuiApp integration: `setNextBindSession`
  + createPty bind-on-seed, `wireLayout()`, relaxed `canCreateSession()` (the B3-Qt relaxation),
  `applyRemoteLayout` (drives `applyLayoutToWindow` with the binding seeder), and `layoutChanged` →
  `applyRemoteLayout` in `ContourGuiApp` (removed the one-tab-per-session `adoptStartupSessions`).
  **Per the user's suggestion, the `AttachController_test` daemon fixture is now a loopback-TCP daemon
  on an ephemeral port — so the whole attach path runs on Windows, not just POSIX.** A new end-to-end
  test drives a real two-reactor TCP attach to a split daemon layout and asserts a 2-pane split tab in
  the GUI's `SessionModel`. **That exercise exposed a real bug in the remote use case:** the two-reactor
  TLS handshake deadlocked because AttachClient's concurrent read+write both called non-reentrant
  `SSL_do_handshake` (single-loop timing hid it). Fixed by serializing `TlsSocket::handshake()` (one
  driver; others park on a gate). TcpEndpoint is TLS-only again; the fixture uses real TLS-over-TCP, so
  every attach test now covers the two-reactor handshake, plus a focused `net` regression test. All
  suites green (net 36/165, muxserver 136/2769, contour_gui 9820). **Remaining: B4 (multi-window),
  B3-Qt authoring verbs (canCreateSession already relaxed), B5 (interop-only tmux polish).**
- 2026-07-22 · Windows/clangcl-release · **B3-Qt tab authoring + incremental reconciliation done.**
  `applyRemoteLayout` is now incremental (realizes each daemon tab not already shown; `isBound` gates
  it), so a tab authored on the daemon appears locally without rebuilding existing tabs.
  `AttachController::requestCreateTab()` sends `CreateTab`; `SessionFactory::requestRemoteTab()`
  (forwarded by `RoutingSessionFactory`, consulted by `TerminalSessionManager::createNewTab`) routes a
  GUI "new tab" to the daemon instead of a local shell. Verified end-to-end over a two-reactor TLS
  attach (`manager.createNewTab` → daemon grows to 2 tabs → reconciler realizes the second). contour_gui
  9827, muxserver 136/2769. **Remaining: split/close authoring (needs a pane→daemon-tab map and an
  ActivatePane verb for multi-pane tabs) + retire `_closedSessions`; B4 multi-window; B5 tmux polish.**
- 2026-07-23 · Windows/clangcl-release · **B3-Qt COMPLETE — split + close authoring done.** Split:
  `SplitPane` now targets a SESSION (daemon activates that pane, then splits — no `ActivatePane` PDU
  needed); `applyRemoteLayout` reconciles WITHIN a tab (remote→`TerminalSession` map via `sessionForPty`,
  find a daemon split with a bound first child + unbound second, split the local pane); GUI splits route
  via `SessionFactory::requestRemoteSplit` (guarded by `isRealizingLayout` so the reconciler's own
  splits build locally). Close: `unbind` authors `closePane` on user close; a SUBTRACTIVE reconciler pass
  closes local panes whose session left the layout (`closeActivePane` after activating the target).
  `_closedSessions` kept (guards the close race). Four new controller tests over the TLS fixture
  (split-reconcile, split-author, close-reconcile, tab-author). contour_gui 9850, muxserver 136/2769,
  net 36/165. **Remaining: B4 multi-window, B5 interop-only tmux polish.**

## Remaining work — turnkey implementation plan (B3-Qt split/close · B4 · B5)

**Done & verified (Windows, real two-reactor TLS attach):** all of WS-A, WS-C, B1, B3-server, **B2
(full layout reconstruction)**, and **B3-Qt tab authoring + incremental (additive, whole-tab)
reconciliation**. The `AttachController_test` fixture is a loopback-TCP+TLS daemon, so every step
below is now Windows-verifiable — no AF_UNIX/POSIX gate. What remains:

- **B3-Qt split/close authoring — needs FINER reconciliation + a cross-layer map (the real feature).**
  Tab authoring works because a whole new tab has no bound leaves; split/close change panes *inside* an
  already-realized tab, which the current whole-tab reconciler skips. Concretely:
  1. **Intra-tab reconciliation.** In `applyRemoteLayout`, for a tab already represented locally, diff
     its daemon leaves against the bound ones: for each **new** daemon leaf, find its bound sibling
     (the split node's other child's leftmost leaf, which IS bound), activate that sibling's local
     pane, `splitActivePane(orientation)`, and bind the new leaf via `setNextBindSession`; for each
     **vanished** bound leaf (no longer in the layout), close its local pane.
  2. **Cross-layer map.** Step 1 needs the local `TerminalSession`/pane for a *remote* session. Add it:
     when `createPty` binds remote session R, record R→pty; expose `AttachController::paneForSession(R)`
     (or have the manager offer `sessionForPty`), so the reconciler can target the right pane.
  3. **Authoring verbs.** `requestSplitPane(actingRemoteSession, vertical)` and
     `requestClosePane(remoteSession)` on `AttachController` (→ the shipped `splitPane`/`closePane`);
     make `SplitPane` carry the target **session** (not just tab) so the daemon `setActivePane`+splits
     the intended pane — no separate `ActivatePane` PDU needed (`ClosePane{session}` already targets a
     session). Route the GUI actions via `SessionFactory::requestRemoteSplit`/`requestRemoteClose`
     hooks (mirroring `requestRemoteTab`), consulted by `TerminalSessionManager::splitActivePane`/close.
  4. **Retire `_closedSessions`.** With subtractive reconciliation the daemon is the source of truth; a
     closed pane leaves the layout and stays gone — the tombstone (and the flattening-era resurrect
     test) can go. Update the resurrect test to the layout-driven close path.
  Tests (Windows, over the TLS fixture): author a split → 2-pane tab; close a pane → back to 1;
  another client's split/close reconciles in.
- **B4 — multi-window.** v1: one GUI window per daemon window in `LayoutState` (the daemon starts with
  one; grow via lifecycle PDUs). Decide the multi-client resize policy (F8): shared grid
  (last-proposal-wins, today) vs per-client server-side viewports.
- **B5 (interop-only, lower priority).** tmux-path polish: F3 ratio/anchor fidelity, GUI→tmux
  `split-window`/`kill-pane`, `%window-renamed`→tab title, `capture-pane` SGR + `-S -` scrollback,
  F6 `%pause` reaction.

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
