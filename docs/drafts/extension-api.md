# Scripting & Extension API

THIS IS A DRAFT DOCUMENT

Design for a sandboxed extension API, hosted on WebAssembly. Supersedes the wishlist in
[#398](https://github.com/contour-terminal/contour/issues/398) by answering the question that
ticket left open: **what API surface makes an extension actually useful?**

- [1. Motivation](#1-motivation)
- [2. Design principles](#2-design-principles)
- [3. Architecture](#3-architecture)
- [4. The interface catalog](#4-the-interface-catalog)
- [5. Manifest and lifecycle](#5-manifest-and-lifecycle)
- [6. Security model](#6-security-model)
- [7. Versioning and compatibility](#7-versioning-and-compatibility)
- [8. Runtime selection](#8-runtime-selection)
- [9. Testing strategy](#9-testing-strategy)
- [10. Distribution and tooling](#10-distribution-and-tooling)
- [11. Phasing](#11-phasing)
- [12. Alternatives considered](#12-alternatives-considered)
- [13. Open questions](#13-open-questions)
- [14. Appendix: interface signatures](#14-appendix-interface-signatures)

---

## 1. Motivation

### 1.1 What changed since #398

The 2020 ticket asked for scriptability "preferably via WebAssembly" and listed three hard
requirements: **zero performance impact when no extension uses a hook**, **crash isolation**, and
**runtime load/unload**. Those requirements still stand. Almost everything else about the answer has
changed, because the architecture underneath moved:

- **Daemon mode** (`src/vthost`, see [internals/vthost.md](../internals/vthost.md)) split the
  terminal into a Qt-free session owner, a presentation client, and a versioned protocol between
  them. An extension host is a *third participant in that same architecture* rather than a bolt-on
  to a monolithic GUI — which is what gives extensions a well-defined answer to "which process am I
  in, and what can I see from there".
- **`Grid` grew a change stream** — stable row ids plus per-line dirty bits, consumed through
  `Grid::forEachLineChangedSince` (`src/vtbackend/grid/Grid.hpp:756`) with a per-consumer
  `GridDeltaCursor`. Built for the daemon's cell tap, it is exactly the event source a
  grid-observing extension needs, and it already costs one byte store per mutation on the hot paths.
- **Shell integration became an interface.** `vtbackend::ShellIntegration`
  (`src/vtbackend/shell/ShellIntegration.hpp:10`) is pure-virtual with precisely the four OSC 133 events,
  and there is a `NullShellIntegration` for "nobody is listening". The single most valuable
  extension surface in this whole document therefore needs *no new hook* — only a fan-out decorator.
- **WASI 0.3.0 shipped on 2026-06-11** with native async in the Component Model (`async func`,
  `stream<T>`, `future<T>`), supported from Wasmtime 43; WASI 1.0 with long-term support commitments
  is targeted for late 2026 / early 2027. WebAssembly-as-plugin-substrate is no longer a bet on a
  future standard.
- **Two peers proved the shape.** [Zellij](https://zellij.dev/news/new-plugin-system/) runs WASM
  plugins in a terminal multiplexer with a user-granted permission cache;
  [Zed](https://zed.dev/blog/zed-decoded-extensions) runs WASM extensions against a versioned,
  WIT-described host API in a desktop application. Neither is speculative any more.

### 1.2 The load-bearing observation

Contour's extension points mostly **already exist**, as data-driven registries with dependency
injection seams. This was not built for extensions; it fell out of the coding guidelines in
[AGENT.md](../../AGENT.md). But it means the extension host's job is not to invent extension points.
It is to let a sandboxed guest **contribute rows to registries that already accept rows**:

| Registry | Location | What its own documentation already says |
| --- | --- | --- |
| `contour::CommandSource` | `src/contour/Command.hpp:33` | *"a sixth kind of command is therefore a new class, not an edit to the palette"* |
| `detail::MenuTable<State>` | `src/contour/ContextMenuTable.hpp` | *"Adding an entry to a context menu is adding one of these to that menu's table, and nothing else"* |
| `StatusLineDefinitions::Item` | `src/vtbackend/screen/StatusLineBuilder.hpp` | a `std::variant` of 16 item kinds; a new kind is a new struct |
| `HintPattern` | `src/vtbackend/input/vi/HintModeHandler.hpp:27` | pattern + action, pure data, with an `Executor` seam |
| `actions::Action` | `src/contour/Actions.hpp` | one variant that the keymap, the command palette **and** every context menu read |
| `vtpty::Pty` | `src/vtpty/Pty.hpp:52` | already a pure-virtual transport interface |
| `vtbackend::ShellIntegration` | `src/vtbackend/shell/ShellIntegration.hpp:10` | already a pure-virtual observer interface |
| `vtworkspace::SessionModel` | `src/vtworkspace/SessionModel.hpp` | a complete tab/pane/window verb set, Qt-free |

The consequence for scope: **milestone 1 is small**, because for several interfaces the work is a
binding and a permission check, not a new mechanism. And the consequence for design: an extension
that registers an action gets a config-bindable key, a command-palette row, and a context-menu
candidate *for free*, because all three read the one registry. That is the data-driven principle in
[AGENT.md](../../AGENT.md) paying a dividend it was not designed to pay.

### 1.3 Driving use cases

Ten extensions, chosen so that between them they exercise every interface proposed in §4. They are
the design pressure for the rest of this document; whenever a decision below looks arbitrary, it is
because one of these needed it.

**1. Block navigator.** Fold a command's output away, jump between commands with a keystroke, show a
red gutter mark next to every command that failed and a duration next to every command that took
longer than a second. Needs `shell` events, `ui.overlay`, `actions`. This is the extension that
proves the point of the whole API: it is most of what a "modern block-based terminal" sells, and in
this design it is a third-party extension that nobody on the Contour team has to maintain.

**2. Error triage.** Recognize compiler, linker, and test-runner output; badge the offending lines;
turn `path/to/file.cpp:42:17` into a click that opens `$EDITOR` at that position; offer
"copy just the failure" in the context menu. Needs `grid` reads, `ui.overlay`, `ui.hints`,
`host.proc`, `input` (clipboard transform).

**3. Secret redaction.** Mask anything that looks like an API token, a private key, or an AWS
credential — in the visible grid *and* in whatever gets copied out of it. Needs `ui.overlay` and a
copy transform. This is the use case that forces overlays to be **render-time decoration data**
rather than grid mutation: the underlying cells must stay intact, because the user still needs
`Ctrl+R` history search and reflow to work on the real text.

**4. Git awareness.** Show branch, dirty state, and ahead/behind in the status line for whichever
pane is focused; put change marks in the gutter for a file being `cat`ed. Needs `shell` events (for
cwd), `ui.statusline`, `host.proc`, `host.time`. A natural *script*-tier extension: forty lines and
a `git` invocation.

**5. Session recorder.** asciinema-style capture and replay, driven from the raw byte tap so it is
byte-exact including images. Needs `vt` (byte tap), `host.fs`. The tap already exists as
`vthost::TappingPty`.

**6. Transport provider.** Make `ssh://host/`, `k8s://ns/pod/container`, `serial:///dev/ttyUSB0` and
`wsl://distro` first-class — openable from the command palette, bindable to a profile, restorable in
a saved layout. Needs `pty` (a provider, not a consumer), `host.net`, `host.secrets`. No other
terminal offers this, and it is the honest answer to #398's "SSH/telnet client" item.

**7. Symbol picker.** #398's own example: a searchable emoji / Unicode / Nerd Font glyph picker that
types the chosen character into the shell. Needs `ui.panel`, `actions`, and input injection.

**8. Visual effects.** #398's other wishlist: a wallpaper, a CRT curvature-and-scanline shader, a
cursor that leaves a trail, inactive panes dimmed. Needs `ui.render`.

**9. Agent bridge.** Watch command blocks; when one fails, offer an explanation; let the user ask
for a command and *propose* it — never run it. Needs `shell`, `ui.panel`, `host.net`, and the
confirmed-injection flow of §6.2. This use case is why §6 is as long as it is: it wants exactly the
three most dangerous capabilities in the system at once.

**10. Collaboration.** On a daemon-hosted session, admit a second client read-only, show its cursor
as presence, and revoke it. Needs `workspace`, `host.net`, and session-side placement — a
client-side extension cannot do this, because the thing being shared outlives every client.

---

## 2. Design principles

Seven rules. Each is justified from something already true about this codebase, because a principle
that is not paid for by an existing constraint tends not to survive the first implementation
compromise.

### 2.1 Extensions contribute data; they do not run inside the terminal

No extension code executes inside `Terminal`, inside the parser, under the terminal lock, or on the
render thread. An extension receives **events** and submits **declarations**: a list of decorated
ranges, a set of palette rows, a status-line item, a registered action. The host applies those
declarations at a point of its own choosing.

This is not merely tidiness. It is what makes the other six rules implementable at all: crash
isolation is meaningless if the crash happens with the terminal mutex held, and "zero cost when
unused" is unachievable if the hook is a virtual call in `writeText`.

### 2.2 No per-cell hooks — ever

Three distinct paths reach the render buffer, and the trivial-line fast path bypasses `renderCell`
entirely. A uniform-SGR line — which is *most* lines — is emitted wholesale without the per-cell
function ever being consulted. A per-cell extension hook would therefore be silently dead on the
common case while appearing to work in tests written against decorated lines.

So the granularity offered to extensions is **line, batch, or semantic event**, never cell. Where an
extension genuinely needs sub-cell precision — the redaction use case wants a character range — it
receives a line and *returns ranges*, and the renderer applies them. The extension never gets a
callback per cell, and consequently the fast path never has to ask whether one is installed.

### 2.3 "Zero cost when unused" is a mechanism, not an aspiration

#398 asked for zero impact on unused endpoints. Three mechanisms deliver it, in order of how much
they carry:

1. **Activation events.** An extension is not loaded until something it declared interest in
   happens. Its *contributions* (actions, palette rows, menu entries, key bindings) come from the
   manifest and are visible to the user before a single WASM page is allocated. This is VS Code's
   model, and it is the reason a user with thirty extensions installed does not pay for thirty
   modules at startup.
2. **Subscription bitmask.** Every hook site is guarded by one test against a `uint64_t` of
   subscribed event kinds. With no extension loaded the mask is zero, the branch is
   perfectly predicted, and the site costs a load the surrounding code was going to do anyway.
   Because the mask is per-*session*, a hook in a session no extension attached to is free even
   while another session pays.
3. **Coalescing at the source.** A grid-change subscriber is served from the existing
   `GridDeltaCursor` machinery at frame granularity, not per mutation. An extension cannot ask to be
   woken per keystroke, because the API has no verb for it.

This is testable, so it will be tested: a `termbench-pro` gate (already a dependency) asserting no
throughput regression in a build with the extension host compiled in and no extension loaded. §9.

### 2.4 Extensions never re-enter the terminal

An event delivered to an extension is delivered *after* the lock is dropped, on a thread that is not
the parser thread, not the render thread, and not the GUI thread. Host calls made from an extension
are queued back and applied at a safe point.

The price of getting this wrong is already documented in this project's history: a synchronous
re-query from a platform callback that read grid state from the wrong thread produced both a
segfault and a freeze, and the pane-teardown crash class comes from exactly this shape — a callback
that outlives the object graph it was reasoning about. An extension is a *much* larger version of
that hazard, because its callback duration is unbounded and third-party.

Corollary: **there is no synchronous "veto" hook.** An extension cannot be asked "may this line be
printed?" — that would put third-party code on the output path with the lock held. Where a decision
genuinely must be gated (§6.2's confirmed injection), the gate is the *user*, asynchronously, not
the extension.

### 2.5 Capability-based security; no ambient authority

Every interface sits behind a permission the user granted to that specific extension. There is no
"trusted extension" tier that skips the checks, and no ambient filesystem, network, process, or
clipboard access. WASI preopens only — an extension's `host.fs` view is the directories it was
handed, and there is no path by which it can name another.

§6 is the full treatment, including the three capabilities that are qualitatively more dangerous
than the rest.

### 2.6 One interface definition; every binding is generated from it

A single IDL — written in WIT shape (§8) — is the source for the host-side stubs, the Rust and C
guest SDKs, the script-tier bindings, the permission table, and the reference documentation. Adding
a host function is adding a row to that IDL.

This is [AGENT.md](../../AGENT.md)'s data-driven principle applied to the API boundary, and the test
it sets for itself is the usual one: *if a nineteenth interface showed up tomorrow, how many places
would need editing?* The answer must be one.

### 2.7 The host is Qt-free and headless-testable

A new module, `src/vtscript`, sits above `vtbackend`/`vtworkspace` and below `contour`, with a
`vtscript_test` Catch2 target like every other module. (Named `vtscript` and deliberately **not**
`vtextension`: `vtbackend::VTExtension` already exists and means something else entirely — which
*vendor* an escape sequence originates from, `XTerm` or `Contour` or none. Two meanings of
"extension" one grep apart, in a document that discusses both, is a confusion worth spending a
better name to avoid.) The GUI-side half of the API (§3.1) is
reached through interfaces the GUI implements, so the host itself never links Qt — the same
discipline `src/vthost` follows, and for the same reason: the interesting logic must be exercisable
without a window.

---

## 3. Architecture

### 3.1 Where an extension runs

Daemon mode already established two homes for terminal state, and an extension must pick one per
component. This is the single most consequential structural decision in the design, because it
decides what an extension can see and how long it lives.

| | **Session-side** | **Client-side** |
| --- | --- | --- |
| Host process | `contour daemon`, or the session-owning half of a standalone GUI | the GUI process |
| Sees | grid & scrollback, VT stream, PTY, layout model, shell semantics | overlays, panels, input gestures, render slots, window chrome |
| Cannot see | pixels, fonts, the window | anything that outlives the window |
| Lifetime | the session's — survives client disconnect and GUI restart | the window's |
| Thread | its own; marshals via `net::EventLoop::post()` (`src/net/EventLoop.hpp:116`) | its own; marshals to the GUI thread |
| Tested by | `vtscript_test`, Qt-free | `contour_gui_test`, offscreen |

Three consequences worth stating explicitly:

- **Session-side extensions work with no GUI at all.** A recorder or an agent bridge keeps running
  on a headless daemon whose last client detached, which is the entire point of daemon mode.
- **Session-side extensions work over the network.** A `contour client` attached over TLS gets the
  extension's behaviour without the extension being installed on the client machine.
- **A GUI-only build still has both sides.** In a standalone GUI, the "session side" is the same
  process; the split is a capability boundary and a threading boundary, not necessarily a process
  boundary. Extensions must not assume otherwise, and the API gives them no way to find out.

An extension with both concerns declares two components (§5) and communicates between them through
its own state, not through the host — the host offers no cross-component channel, because that
channel would have to work across a socket in one deployment and not in another.

### 3.2 Two guest tiers, one ABI

**Tier 1 — compiled guests.** Rust, C, C++, Zig, or anything else targeting `wasm32`, linked against
a generated SDK. This is the tier for the transport provider, the recorder, the render effects.

**Tier 2 — scripted guests.** A JavaScript (or Lua) interpreter, itself compiled to `wasm32` and
shipped with Contour, loaded as a guest that re-exposes the host ABI to a plain
`~/.config/contour/extensions/<id>/main.js`. This is the tier for the git status line, a keybinding
that runs three host calls, a hint pattern with a custom handler. #398 said "scripting"; this is what
makes that word true.

The host cannot tell the two apart, and that is the design:

- **Identical sandbox.** The interpreter runs *inside* WASM rather than being linked natively,
  specifically so that the convenient tier is not the insecure tier. A native QuickJS would have
  ambient access to the host process; a WASM QuickJS has exactly the capabilities its manifest was
  granted.
- **Identical permissions and activation events.** There is no "it's only a script" shortcut. A
  script that wants to read the grid asks the user, in the same dialog, with the same wording.
- **Identical event delivery.** Both tiers see the same events with the same coalescing and the same
  deadline.
- **The cost is real but bounded.** An interpreter module is on the order of a megabyte, adds a
  cold-start compile (mitigated by an AOT cache keyed on module hash), and adds a second dispatch
  hop per host call. Acceptable, because extension work is event-driven: the git status line makes
  one host call a second, not a million.

The IDL generating both tiers' bindings also leaves the door open to an **out-of-process** tier —
the same verbs over the existing `vthost` socket, so a Python or Go extension needs no WASM at all.
That is deliberately **not** in scope here (§4.4 non-goals); the note matters only because it
constrains the IDL to stay serializable, which it must be anyway.

### 3.3 Threading, cancellation, and deadlines

Host calls are `coro::Task`-shaped on the session side, matching `net::EventLoop`, `net::ISocket`
(`src/net/ISocket.hpp:37`), and WASI 0.3's native async — a guest's `await` on a socket read suspends
that guest without blocking the loop or any other extension.

Each callback carries a wall-clock deadline. **Do not implement that deadline with
`coro::whenAny`**: it swallows parent cancellation — `await_resume` returns a winner index instead of
throwing `OperationCancelled`, so a host shutting down mid-await would see the timeout race complete
normally instead of unwinding. The deadline belongs in the runtime's own interruption mechanism
(epoch interruption or fuel, §8), which unwinds the guest rather than racing it.

A guest exceeding its deadline is **suspended, not killed** on first offence, and its pending
declarations are dropped; repeated offences unload it (§6.4). The distinction matters because a slow
extension on a loaded machine is not the same thing as a malicious one, and the user experience of
"your extension was disabled" should be reserved for the latter.

### 3.4 The event pipeline

```
                  session side                        │        client side
                                                      │
  PTY ──► vtparser ──► Screen/Grid ──┐                 │
                                     │                 │
              ShellIntegration ◄─────┤ (fan-out        │
              (fan-out decorator)    │  decorator)     │
                     │               │                 │
                     ▼               ▼                 │
              ┌──────────────────────────────┐         │   ┌──────────────────────┐
              │  ExtensionHost (session)     │         │   │ ExtensionHost (GUI)  │
              │  • subscription mask         │         │   │ • overlay store      │
              │  • coalescing queue          │         │   │ • panel models       │
              │  • per-guest budget/deadline │         │   │ • CommandSource impl │
              └──────────────┬───────────────┘         │   └──────────┬───────────┘
                             │ own thread              │              │ own thread
                    ┌────────┴────────┐                │      ┌───────┴────────┐
                    │  guest modules  │                │      │ guest modules  │
                    └─────────────────┘                │      └────────────────┘
```

Events flow in one direction; declarations flow back and are applied by the host at a safe point.
The two hosts are the same code with different capability tables.

---

## 4. The interface catalog

Eighteen interfaces in five families. Each subsection gives purpose, permission, placement, what it
costs when nobody uses it, and which existing type it is built on. Signatures are in §14.

### 4.1 Terminal & session

#### `contour:terminal/workspace`

**Permissions:** `workspace:read`, `workspace:write` · **Placement:** session · **Cost unused:** nil
(pull API plus a coalesced change event)

Enumerate and manipulate the window/tab/pane tree: create, split, close, focus, move, resize, zoom,
swap; read and set tab titles and tab colours; load and save layouts.

This maps almost one-to-one onto `vtworkspace::SessionModel` (`src/vtworkspace/SessionModel.hpp`),
which is already Qt-free and already has the whole verb set — `createTab`, `closeTab`, `activateTab`,
`moveTab`, `moveTabToWindow`, `closePane`, `setActivePane`, `focusDirection`, `setPaneRatio`,
`resizeActivePane`, `toggleActivePaneOrientation`, `swapActivePane`, `moveActivePane`,
`toggleActivePaneZoom`, `setTabTitle`, `setTabColor`. The extension interface adds nothing but a
permission check and identity mapping.

**Identity is by opaque id, not index.** `WindowId`/`TabId`/`PaneId` are already stable; indices are
not, and an extension that stored index 3 across a tab close would act on the wrong pane. The API
never exposes an index as an addressing token, only as an ordering fact.

Grid sizes come from `clampedTotalPageSize()`, never re-derived: a client area is not the pane grid,
and an extension that computes its own clamp will disagree with the renderer.

Use cases: project-aware layouts (#1 pane-per-service on `cd` into a repo), the collaboration
extension, a pane jump-list.

#### `contour:terminal/grid`

**Permissions:** `grid:read` (high, §6.3), `grid:write` (high, §6.2) · **Placement:** session ·
**Cost unused:** nil for reads; the change event rides the existing `GridDeltaCursor`

- **Pull reads.** Viewport, an arbitrary line range, or the whole scrollback, as plain text or as
  cells carrying SGR, hyperlink ids, image coverage, grapheme-cluster extras and OSC 66 scale.
- **Logical vs physical.** Every range is addressable either way, and the distinction is explicit in
  the type, because it is the single most common source of bugs in this area: a mark names the
  *logical* line and must survive reflow, while a decoration names what is on screen.
- **Stable row ids.** A subscriber follows changes through a `GridDeltaCursor`, receives
  `resync-required` when row identity was rebuilt, and re-snapshots. Same contract the daemon's
  clients get, so it is already proven under scroll, margin scroll, and eviction.
- **Selection.** Read the current selection, replace it, clear it. Backed by
  `Terminal::extractSelectionText()` and `setSelector()`.
- **Search.** Run a search, iterate matches, jump. Backed by `Terminal::search()` /
  `searchNextMatch()`.
- **Writes are a separate, high permission** and by default are refused; see §6.2. Almost every use
  case that reaches for a grid write actually wants `ui.overlay` instead.

Use cases: error triage, redaction, the recorder's replay verification, the agent bridge's context.

#### `contour:terminal/shell`

**Permission:** `shell:observe` · **Placement:** session · **Cost unused:** nil — the existing
`NullShellIntegration` stays installed until someone subscribes

The most valuable interface in this document, and the cheapest to build, because
`vtbackend::ShellIntegration` (`src/vtbackend/shell/ShellIntegration.hpp:10`) is already a pure-virtual
observer with exactly the four OSC 133 events: `promptStart`, `promptEnd`, `commandOutputStart`
(carrying the command line, when the shell sent one), `commandFinished(exitCode)`. Subscribing means
swapping the null implementation for a fan-out decorator.

On top of those raw events the interface offers the semantics extensions actually want:

- **`command-finished`** with exit code, wall-clock duration, the command line if known, the cwd, and
  the logical line range the output occupied.
- **Block queries** — the block at a position, the last N blocks, a block's prompt / command /
  output text. Backed by `scanCommandBlocksBackward` and `textOf`
  (`src/vtbackend/shell/CommandBlocks.hpp`), which are already written against a
  `CommandBlockLineSource` DI seam and are therefore already testable without a Grid.
- **The live prompt span** — where the shell's prompt is *right now*, or why there isn't one.
  Backed by `Terminal::livePromptSpan()`, which returns
  `std::expected<LivePromptSpan, PromptRegionError>` and says "an application is up, there is no
  prompt" as data rather than as a guess.
- **Block navigation** as a host call, so several extensions agree on what "next block" means.

Ranges are **logical** line positions, reflow-invariant, matching how OSC 133 marks are stored.

Use cases: block navigator, git awareness, agent bridge, error triage, long-command notification.

#### `contour:terminal/vt`

**Permissions:** `vt:claim`, `vt:tap-sequences`, `vt:tap-bytes` (high) · **Placement:** session ·
**Cost unused:** claim is a hash lookup on an otherwise-unhandled sequence — i.e. free; the taps are
mask-gated

Three distinct things that are easy to conflate:

**(a) Claiming private sequences.** An extension registers a handler and thereby gets *its own
protocol*: a program running in the terminal can talk to it. This is the surface that makes
extensions composable with the shell and with TUI applications, rather than merely bolted onto the
UI. `hx --terminal-features`, a build tool reporting progress, a REPL asking for a picker — all of
that becomes possible without Contour shipping a sequence for each.

The namespace question matters, because OSC numbers are a global unallocated space that every
terminal squats in (Contour itself holds 66 and 888; iTerm2 has 1337; kitty has 99 and 5522; urxvt
has 777). Handing extensions numeric OSC ids would manufacture collisions between extensions *and*
with future standard sequences. So:

- **Primary: APC, namespaced by extension id.** `APC x <ext-id> ; <payload> ST`. APC is unallocated
  in the DEC manuals, ignored by xterm, and Contour's parser already recognizes it
  (`Parser.hpp` states `APC_String`/`APC_Start`/`APC_Put`/`APC_End`) and already collects a bounded
  body that it drops when overlong (`SequenceBuilder.hpp:183`). A string extension id cannot collide by
  accident, and `x` keeps clear of kitty's `APC G`.
- **Fallback: one reserved OSC id, namespaced identically** — `OSC 889 ; <ext-id> ; <payload> ST` —
  for tooling that can only emit OSC. One number to register with the community, not a range to
  squat. (The exact number is §13.)
- **Replies** carry a correlation token, so a program can request-and-await rather than fire-and-hope.
- Extensions **cannot claim standard sequences.** An extension that shadowed `OSC 52` or `DECSET`
  would break conformance and turn every VT bug report into an extension bisect. Observation of
  standard sequences is `vt:tap-sequences`; interception is not offered.

**(b) The parsed-sequence tap.** Read-only notification of dispatched sequences, mask-gated. This is
#398's "VT sequence analytics": a live inspector, a conformance recorder, a "what does this
application actually use" profiler. It composes with the existing trace mode rather than replacing
it.

**(c) The raw byte tap.** The pre-parser byte stream, which is what byte-exact recording requires —
images included. Backed by `vthost::TappingPty`. This is a **high** permission: the byte stream
contains everything the user typed, including passwords, before any protected-mode reasoning has
happened.

Use cases: recorder, inspector, TUI-to-extension protocols, image-protocol adapters.

#### `contour:terminal/pty`

**Permission:** `pty:provide` · **Placement:** session · **Cost unused:** nil — a scheme table lookup
at session creation

An extension **implements** a transport rather than consuming one, and a URL scheme becomes a
first-class session kind: openable from the command palette, referenceable from a profile,
restorable from a saved layout, and — because the session lives in the daemon — reconnectable after
the GUI restarts.

`vtpty::Pty` (`src/vtpty/Pty.hpp:52`) is already the right interface: `start`, `read` into a
`crispy::buffer_object`, `write`, `resizeScreen`, `pageSize`, `waitForClosed`, `wakeupReader`, plus a
`PtySlave`. The guest side is the inverse of the normal binding direction — the host calls into the
guest — so it needs a genuine async read (WASI 0.3's `stream<u8>`, or a poll-shaped fallback).

Two rules keep this honest: a provider **must** honour `resizeScreen` (a transport that ignores
`SIGWINCH` semantics produces the classic "vim thinks the window is 80x24" bug), and a provider that
returns no bytes for a bounded interval after `start` is reported as failed to connect rather than
hanging the tab.

Use cases: `ssh://`, `k8s://`, `serial://`, `wsl://`, container exec, cloud shells.

### 4.2 Input & commands

#### `contour:terminal/actions`

**Permission:** `actions:register` · **Placement:** either · **Cost unused:** nil — declared in the
manifest, resolved at binding time

An extension declares named actions in its manifest; each becomes:

- bindable in `contour.yml` like any built-in action (`input_mapping`, remembering that an entry
  without `mods:` is silently dropped — "no modifiers" is `mods: []`);
- a command-palette row, via `ActionCommandSource`'s existing merge;
- a candidate context-menu row, with a predicate over the click context.

All three because `actions::Action` is one registry that all three read. Invoking a declared action
is also what triggers lazy activation (`onAction:<id>`), so the user sees and can bind an
extension's actions before its code has ever been loaded.

Actions may carry typed parameters, mirroring how `HintMode` carries patterns and `SendChars`
carries characters — that is what makes them useful from a keybinding rather than only from a menu.

#### `contour:terminal/input`

**Permissions:** `input:observe`, `input:claim`, `input:transform-clipboard`,
`terminal:inject-input` (crown jewel, §6.2) · **Placement:** client (claims), session (injection) ·
**Cost unused:** claims are resolved in the existing binding table; transforms are mask-gated

- **Claim chords and gestures** not otherwise bound, and get them as events. Contour's own
  resolution quirks apply and are documented for extension authors — notably that shifted
  punctuation arrives as the shifted codepoint, so `Ctrl+Shift+,` must also be matched under its
  unshifted base.
- **A modal input consumer**, peer to Vi mode: while an extension's mode is active it receives keys
  first and decides what to pass through. This is how a modal extension (a jump mode, a chord-driven
  navigator) works without hijacking the global keymap. One consumer at a time, and the status line
  says which.
- **Clipboard transforms** on copy and on paste: strip ANSI, redact secrets, rewrite
  WSL↔Windows paths, warn on a multi-line paste. Paste transforms run *before* the bracketed-paste
  wrapper and cannot see other extensions' output, so ordering is declared, not emergent.
- **Input injection** writes to the PTY. This is arbitrary code execution as the user; §6.2.

### 4.3 Presentation

#### `contour:ui/statusline`

**Permission:** `ui:statusline` · **Placement:** client · **Cost unused:** nil — a variant member
nobody constructs

A new `StatusLineDefinitions::Item` alternative, `Extension { id, slot }`, referenced from the same
`{...}`-interpolated status-line string users already write. The extension pushes text plus styling
when it has something to say; the host renders it through `serializeToVT` like every other item and
applies the same width budget.

The existing `Command` item — which shells out — is the precedent, and a large part of what
extensions do here is the same job without a fork per refresh.

#### `contour:ui/overlay`

**Permission:** `ui:overlay` · **Placement:** client · **Cost unused:** nil — an empty list

The most reusable interface in the document, and the one whose shape most needs defending.

An extension submits **decoration data** anchored to grid positions, and the renderer draws it:

- **styled ranges** — underline (with style and colour), strike-through, box, background tint,
  foreground override, dim;
- **gutter marks** — a glyph or colour bar in the margin, addressed by logical line;
- **badges** — a short label at the end of a line or block (`1.4s`, `exit 1`, `↑2`);
- **block affordances** — collapse/expand state for a command block, so the block navigator can fold
  output without touching the grid;
- **region tints** — a whole block or line range shaded, for "this command failed".

Why declarative rather than a draw callback:

1. **Security.** A drawing extension can paint a convincing `[sudo] password for user:` prompt.
   Decoration data cannot introduce text that is not on screen, so the spoofing surface closes by
   construction. Free-form screen writes exist, but as a separate high permission (§6.2).
2. **Performance.** An empty declaration list costs nothing; a non-empty one costs a merge into a
   pass the renderer was going to make anyway. A draw callback would put third-party code on the
   render thread — forbidden by §2.4 — or force a round trip per frame.
3. **Correctness under reflow.** Anchoring to *logical* positions means a resize re-chops the
   physical lines and the decorations follow, with no extension involvement. An imperative overlay
   in device pixels would be wrong the moment the font size changed.
4. **It composes.** Two extensions decorating the same line merge; two extensions drawing on the
   same line fight.

Ordering when declarations conflict on one cell is deterministic and documented (load order, then
declaration order), not last-writer-wins.

One open problem: decorations are declared in logical order, and cells go to *visual* order at
`RenderBufferBuilder`. For an RTL run, a range that is contiguous logically is not contiguous
visually. §13.

#### `contour:ui/panel`

**Permission:** `ui:panel` · **Placement:** client · **Cost unused:** nil

A **declarative widget tree** — list, filterable list, text input, static text, key-value table,
progress, image — rendered by Contour's own QML, in one of a few slots: a modal quick-pick, a docked
side panel, a transient popover. The extension sends a model and receives selection and input
events.

Not arbitrary QML, and not a webview. The reasons are the same as for overlays, plus one more: a
declarative model is what lets the panel be themed, keyboard-navigable, accessible, and consistent
across extensions. The existing command palette (`CommandPaletteModel`, with vim-style navigation
and match-position highlighting) is the proof that this vocabulary is sufficient for the
interesting cases.

Popup placement is a known trap here — `Popup.margins` defaults to `-1`, so an explicitly-placed
popup is never clamped to the window, and dispatching from `onClosed` re-enters `open()`. The host
owns placement and dispatch so extensions cannot reintroduce either.

Use cases: symbol picker, agent bridge chat, a git-branch switcher, a "pick a session to attach"
list.

#### `contour:ui/palette`

**Permission:** `ui:palette` · **Placement:** client · **Cost unused:** nil — a source that is only
queried when the palette opens

An extension becomes a `contour::CommandSource` (`src/contour/Command.hpp:33`) — the interface whose
documentation already anticipates this: *"a sixth kind of command is therefore a new class, not an
edit to the palette."* It is asked for rows each time the palette opens, so rows can depend on live
state, and it participates in the existing dedup-by-id precedence.

Use cases: recent directories, git branches, k8s contexts, saved SSH hosts, "attach to session".

#### `contour:ui/hints`

**Permission:** `ui:hints` · **Placement:** client · **Cost unused:** nil — patterns are scanned only
while hint mode is active

Register `HintPattern`s (`src/vtbackend/input/vi/HintModeHandler.hpp:27`) with a custom handler through the
existing `HintModeHandler::Executor` seam: recognize `ABC-123` and open the ticket, recognize
`file.cpp:42:17` and open the editor there, recognize a container id and offer `exec`. Patterns join
`builtinPatterns()` rather than replacing them.

#### `contour:ui/notify`

**Permission:** `ui:notify` · **Placement:** either · **Cost unused:** nil

Post, replace, and close desktop notifications, with activation callbacks. Backed by
`vtbackend::DesktopNotification` and `contour::NotificationRouter`, which already owns identifier
mapping, replace-in-place, and urgency policy — so an extension's notifications behave like OSC 99's
and can replace each other properly.

#### `contour:ui/render`

**Permission:** `ui:render` · **Placement:** client · **Cost unused:** nil — an unfilled slot is a
pass not recorded

The riskiest interface, and the one most likely to slip a milestone. Scoped as **slots with declared
uniforms**, not GPU access:

- **background layer** — a still image, an animated sequence, or a fragment shader behind the cells,
  with the terminal's own background compositing rules preserved;
- **cursor decoration** — a shader or sprite for the cursor, including motion (the trail);
- **post-process** — one full-frame effect over the composed terminal (CRT curvature, scanlines,
  bloom);
- **pane treatment** — per-pane tint/dim, so "dim inactive panes" is not a special case in the core.

An extension declares uniforms and their types in the manifest and updates them per frame at most;
shader source is validated and compiled by the host. No buffer uploads, no arbitrary draw calls, no
access to another pane's texture.

Two hazards, both already known here: image gap-fill has to ride the image pass because the rect
pass is recorded before the text pass and draw *order* is the only z-ordering — so slot ordering is
fixed by the host, not chosen by extensions. And the renderer's coordinates are device pixels with
DPR pre-scaling, so uniforms are handed to extensions in **cells and normalized coordinates**, never
in pixels.

#### `contour:ui/text`

**Permission:** `ui:text-provider` · **Placement:** client · **Cost unused:** nil — consulted only on
a fallback miss

A glyph provider consulted when the font stack has no glyph for a cluster: render an icon for a
codepoint, supply an SVG glyph, map a private-use range onto a bundled asset. Backed by the
`text_shaper` abstraction. Deliberately last in the chain, so an extension cannot slow down or alter
ordinary text.

### 4.4 Host services

All of these are capability-scoped with no ambient authority. Each is refused by default.

| Interface | Permission | Offers | Built on |
| --- | --- | --- | --- |
| `host.fs` | `fs:read`/`fs:write` per preopened root | Read/write within the extension's own data directory plus user-granted roots. Paths are handles, not strings resolvable upward | WASI preopens |
| `host.proc` | `proc:spawn` with an argv-0 allowlist | Two shapes only: a captured-output child (`git status --porcelain`), or a PTY-backed session that becomes a visible pane. No fork, no exec of a path the manifest did not name | `vtpty` |
| `host.net` | `net:connect` per host:port pattern, `net:listen` per port | Connecting and listening sockets with TLS. Patterns are declared in the manifest and shown at grant time | `net::ISocket`, `net::Tls` |
| `host.secrets` | `secrets` | Read/write the OS keychain under a per-extension namespace. An extension cannot name another's secret | new |
| `host.store` | none (own namespace) | A key-value store, plus the extension's own config section — schema declared in the manifest and auto-rendered in the GUI settings page | `reflection-cpp` (already a dependency), `GuiConfigStore` |
| `host.time` | none | Monotonic clock, wall clock, one-shot and repeating timers with a minimum interval | `net::EventLoop` timers |
| `host.log` | none | Structured logging under the extension's own `logstore` category, so `contour --debug ext.<id>` works exactly like `vt.parser` | `crispy::logstore` |
| `host.clipboard` | `clipboard:read`/`clipboard:write` | Read and write the selection/clipboard. Read is separate from write and much more sensitive | existing clipboard plumbing |

Note what `host.store` and `host.log` do **not** need: a permission. An extension's own storage and
its own log category are not authority over anything the user cares about, and requiring a grant for
them would train users to click through grant dialogs — which is the failure mode that makes the
grants that matter meaningless.

### 4.5 Explicit non-goals

Stated as non-goals so they do not get re-litigated per pull request:

- **No per-cell callbacks.** §2.2.
- **No parser replacement or sequence interception.** Observation yes, substitution no. VT
  conformance is a product property, not an extension's to change.
- **No synchronous hooks on the render or parser path.** §2.4. Consequently no veto hooks.
- **No free-form grid writes by default.** Overlays and panels instead; free-form writes are a
  separate high permission that most extensions should never request.
- **No ambient filesystem, network, or process access.** §4.4.
- **No arbitrary QML, no webview, no GPU access.** §4.3.
- **Config stays YAML.** This is not a config scripting language. An extension may *contribute*
  settings; it does not evaluate the config file.
- **No out-of-process extension transport in this iteration.** §3.2 — the IDL keeps the door open;
  the door is not part of this design.
- **No cross-extension API.** Extensions do not call each other. Composition happens through the
  host's registries (two extensions can decorate the same line) or through a private sequence
  (§4.1), both of which the user can reason about.

---

## 5. Manifest and lifecycle

### 5.1 `extension.yml`

The manifest is the contract, and it is deliberately verbose about *why* each permission is wanted,
because that string is what the user reads in the grant dialog.

```yaml
id: org.example.block-navigator      # reverse-DNS, immutable, the namespace for everything
name: Block Navigator
version: 1.2.0
api_version: 0.1                     # the contour:terminal world version, §7
license: Apache-2.0
homepage: https://example.org/block-navigator

components:
  - id: core
    tier: wasm                       # wasm | script
    placement: session               # session | client
    module: block-navigator.wasm
  - id: ui
    tier: script
    placement: client
    module: ui.js

permissions:
  - name: shell:observe
    reason: To know when a command starts and finishes.
  - name: ui:overlay
    reason: To draw the fold markers and the exit-code gutter.

activation:
  - onShellIntegration                # first OSC 133 seen in a session
  - onAction:org.example.fold-block

contributions:
  actions:
    - id: org.example.fold-block
      title: Fold Command Output
      params: { block: block-ref }
    - id: org.example.next-block
      title: Go to Next Command
  keybindings:
    - action: org.example.next-block
      key: Down
      mods: [Ctrl, Shift]
  menu:
    - surface: terminal
      action: org.example.fold-block
      when: over-command-block
  status_line:
    - slot: duration
  settings:
    fold_threshold_lines:
      type: integer
      default: 20
      description: Fold output longer than this many lines.
```

Two properties of this file carry weight out of proportion to their size:

- **Contributions are declarative**, so the palette, the menus, the keymap, and the settings page can
  show an extension's surface *before* its code loads. That is what makes lazy activation invisible.
- **`activation` is a whitelist of triggers**, not a lifecycle hook. There is no `onEveryKeystroke`,
  because offering one would make §2.3 unachievable.

Activation triggers: `onStartup` (discouraged, and the installer says so), `onSession`,
`onShellIntegration`, `onCommandFinished`, `onSequence:<ext-id>`, `onAction:<id>`,
`onCommand:<palette-id>`, `onProfile:<name>`, `onScheme:<scheme>`, `onFileType:<glob>` (for a cwd
match), `onKeybinding:<binding>`.

### 5.2 Lifecycle

```
discovered ──► validated ──► contributions registered ──► (idle)
                                                            │ activation event
                                                            ▼
                                              instantiated ──► activated ──► running
                                                                              │
                          deactivate (session closes / user disables / budget) │
                                                                              ▼
                                                                    drained ──► unloaded
```

- **Validated** means: manifest schema, module magic and imports (an import the world does not
  declare is a hard failure at install time, not a trap at runtime), and permission names.
- **Activated** gets a bounded budget for its `activate` call; overrunning it is a load failure with
  a message naming the extension, not a hang.
- **Drained** matters for correctness: an unloading extension's declarations are withdrawn *before*
  its memory goes away, so the renderer never holds a decoration list belonging to a dead guest.
  This is the same discipline the pane-teardown crash class taught — re-check inside the posted
  lambda, do not assume the object graph survived the hop.
- **Runtime load/unload**, per #398, falls out of this: nothing about a session depends on which
  extensions were loaded when it started.
- **Dev mode** (`contour extension dev ./`) watches the module and re-runs
  deactivate → instantiate → activate on change, with permissions granted from a
  `--grant` flag so an author is not clicking dialogs on every rebuild.

---

## 6. Security model

An extension API is a privilege-escalation surface pointed at the program in which users type
passwords, hold SSH agent sockets, and run `kubectl delete`. This section is long because the
failure modes are not hypothetical.

### 6.1 The permission model

Capability-based, no ambient authority, refused by default. A permission is granted by the user to a
*specific extension version*, persisted in a grant store, revocable individually, and re-prompted
when an update requests something new. A denied permission surfaces to the guest as
`result<_, permission-denied>` — never as a silent empty answer, because an extension that cannot
tell "denied" from "nothing there" will paper over the denial and behave incomprehensibly.

Three capabilities are qualitatively worse than the rest and get their own treatment.

### 6.2 Crown jewel #1: writing is executing

**Writing to the PTY is arbitrary code execution as the user.** `sendChars`-equivalent capability is
not "input automation"; it is a shell. There is no sandbox behind it — the bytes land in a shell
running with the user's full authority, agent sockets, cloud credentials and all.

Therefore `terminal:inject-input` is:

- **separate from every other permission**, and never implied by any of them;
- **worded in the grant dialog as what it is** — "run commands as you", not "send input";
- for anything but a locally-developed extension, reachable **only through the confirmed-injection
  flow**: the extension calls `propose-input(text, rationale)`, the user sees exactly the bytes and
  confirms, and the host writes them. The extension never learns whether the user confirmed quickly
  or edited the text first, so it cannot tune a social-engineering loop.

The agent-bridge use case is the reason this flow exists rather than a blanket grant, and it is
strictly better for that use case anyway: a proposal the user edits is more useful than a command
that just ran.

**Writing to the display is spoofing.** An extension that can place arbitrary text in the grid can
render `[sudo] password for you:` and harvest the answer, or fake a git status, or fake this
terminal's own permission dialog. Hence the whole shape of §4.3: overlays add *decoration* to text
that is already there, panels are visibly extension-owned chrome, and free-form grid writing is a
separate high permission that the vast majority of extensions must never request. Extension-owned
panels carry a persistent, non-suppressible attribution marker for the same reason.

### 6.3 Crown jewel #2: reading the grid reads secrets

`grid:read` and `vt:tap-bytes` see whatever the user typed and whatever a program printed: pasted
credentials, `AWS_SECRET_ACCESS_KEY=` in a `history` dump, a token echoed by a misbehaving CLI. So:

- Both are **high** permissions with explicit wording about screen content.
- They **honour protected mode**: cells the application marked protected, and input arriving while
  input protection is engaged, are elided rather than delivered. The `ToggleInputProtection` action
  and DECSCA/SPA-EPA protected content are the existing mechanisms; the extension API is a consumer
  of them, and one that must not be able to opt out.
- **The combination is the risk, and the UI must say so.** `grid:read` alone is a local privacy
  question. `grid:read` plus `net:connect` is an exfiltration channel — a keylogger with a delivery
  mechanism. The grant dialog for the second permission names the first explicitly ("this extension
  can already read your screen; allowing network access lets it send that content to
  `api.example.com`"). Listing permissions one per line, each individually reasonable, is how real
  permission systems fail.
- **Redaction is not a defence.** The redaction use case (#3) is a *display* feature; it does not
  and cannot restrict what another extension reads.

### 6.4 Crown jewel #3: resource exhaustion and crash isolation

#398 required that a misbehaving extension not take the terminal down. Concretely:

- **Memory**: a per-instance ceiling; an allocation past it traps in the guest, which the host
  reports and the terminal survives. WASM's linear memory means the guest cannot corrupt host memory
  at all — this is the property that makes the whole design tenable, and the main thing a native
  `dlopen` plugin system could never offer.
- **CPU**: epoch interruption or fuel, so a `while(true)` in a guest is interrupted rather than
  livelocking a thread.
- **Wall clock**: a per-callback deadline (§3.3). First overrun suspends and drops pending
  declarations; repeated overruns unload with a user-visible message naming the extension.
- **Event queue depth**: a bounded per-extension queue. A guest that cannot keep up gets
  **coalesced, then dropped**, and is told it was dropped — never allowed to apply back-pressure to
  the terminal. An extension must not be able to make the terminal feel slow.
- **Trap containment**: a trap unwinds the guest, marks the instance poisoned, withdraws its
  declarations, and reports. No terminal state is inconsistent afterwards, because the guest never
  held a lock or a half-applied mutation — §2.1 is what buys that.

### 6.5 Trust tiers and distribution integrity

- **Local development** — an unsigned module in a dev-mode path. Full permission set available, with
  an unmistakable indicator that a development extension is loaded, because "I'll just try this
  `.wasm` a stranger sent me" must not be a smooth path.
- **Registry** — signed, with the manifest's permission set covered by the signature so an update
  cannot silently widen it.
- **Verified** — a manually reviewed subset; the only tier permitted to request
  `terminal:inject-input` non-interactively, and even then the user grants it.

Grants are per-extension-version-and-permission, listable and revocable through
`contour extension permissions`, and stored outside the extension's own reach.

---

## 7. Versioning and compatibility

Extensions are third-party, which makes this the one place where the design must *not* copy the
daemon protocol. `vthost`'s handshake is deliberately an **exact match** — a peer one version
away is refused — and that is correct there, because both ends are the same build (which is also why
its `CodecVersion` need not move at all while the protocol is unreleased). Extensions cannot be the same build as the terminal, so this
handshake **negotiates**.

- **Per-world semantic version.** `contour:terminal@0.1.0` and `contour:ui@0.1.0` version
  independently, so a UI addition does not invalidate every session-side extension.
- **Additive within a major.** New functions, new record fields with defaults, new enum cases at the
  end. A guest built against 0.1 keeps running on 0.4.
- **Unknown-value tolerance in both directions**, following the `Invalid{ident}` precedent in
  `vthost/proto`: an enum case a guest does not know decodes to a reserved "unknown" case that it can
  ignore, rather than trapping. Forward compatibility as data, not as an error.
- **Deprecate, then remove at a major**, with `contour extension list` flagging extensions using
  deprecated functions so a user learns before an upgrade breaks something.
- **Range declaration.** A manifest declares the range it supports; refusing to load is a clear
  message at install time, not a mystery at runtime.

The reciprocal obligation is on us: an interface promoted out of experimental is a compatibility
commitment. §11 therefore keeps everything after M1 explicitly experimental until it has a real
third-party consumer, because an API's second consumer is the one that reveals what the first one got
away with.

---

## 8. Runtime selection

### 8.1 Candidates

| | **Wasmtime** | **WAMR** | **wasmi** | **wasm3** |
| --- | --- | --- | --- | --- |
| Language | Rust (C API, `wasmtime-cpp`) | C | Rust | C |
| Embedding from C++ | C API + C++ header wrapper | native | needs a Rust shim | native |
| Speed | JIT (Cranelift), AOT cache | interpreter / AOT / JIT | interpreter | interpreter |
| Binary size | large (tens of MB) | small (~100 kB core) | small | tiny (~64 kB) |
| Fuel / epoch limits | both | limited | fuel | no |
| Component Model | Rust API yes; **C API incomplete** | no | no | no |
| WASI | 0.2 / 0.3 | p1 (+partial p2) | p1 | minimal |
| Used by | Zed, many | embedded | Zellij | embedded |

### 8.2 The constraint that decides it

**Wasmtime's Component Model support in the C API is still incomplete** (upstream
[#8036](https://github.com/bytecodealliance/wasmtime/issues/8036),
[#6987](https://github.com/bytecodealliance/wasmtime/issues/6987),
[#11437](https://github.com/bytecodealliance/wasmtime/issues/11437)). Contour is C++. So the
attractive design — WIT plus `wit-bindgen`, exactly as Zed does it — is not reachable from C++ today
without either waiting on upstream or introducing a Rust shim crate and a Rust toolchain into the
build. Neither is acceptable as a dependency of milestone 1.

### 8.3 Recommendation

**Wasmtime via the C API, core modules and WASI p1, with a `reflection-cpp`-generated ABI over
linear memory — and the IDL written in WIT shape from day one.**

- **Core modules, not components.** The host↔guest calls are `(ptr, len)` pairs into the guest's
  linear memory carrying a serialized message, exactly the shape Zellij uses — except with
  `reflection-cpp`, already a Contour dependency (`src/crispy/CMakeLists.txt`), instead of protobuf.
  That means **no new codegen dependency** and a serialization idiom identical to the config
  reader's.
- **The IDL is WIT.** Interfaces are authored as WIT (§14) and the ABI is generated from it. The
  generator emits host stubs, guest SDK bindings, the permission table, and documentation today; when
  the C API's component support lands, the same WIT files drive real components and the hand-rolled
  codec is deleted. The migration is mechanical because the interface descriptions never were the
  hand-rolled part.
- **Wasmtime, not WAMR**, despite the size, for epoch interruption plus fuel, an AOT cache, the
  strongest sandbox track record, the clearest path to components, and Zed's demonstration that it
  is viable in a desktop application. Binary size is the price; the fallback if it proves
  unaffordable on a target platform is WAMR behind the same generated ABI — which is precisely why
  the ABI is generated and not hand-written per runtime.
- **WASI 0.3's native async is the target, not the starting point.** It shipped 2026-06-11 in
  Wasmtime 43 and maps cleanly onto `coro::Task`; but it arrives through the component path, so M1
  uses a poll-shaped fallback and adopts `stream<T>`/`future<T>` with the component migration.

### 8.4 New third-party dependencies

[AGENT.md](../../AGENT.md) requires justification for new dependencies. This design proposes two:

1. **A WASM runtime.** Unavoidable — the sandbox *is* the feature, and hand-rolling one is not a
   serious proposition. Justification: it is the only mechanism that delivers #398's crash isolation
   and untrusted-code requirements simultaneously. Mitigation: reached through one interface in
   `src/vtscript`, so it is replaceable; a build without it drops the module entirely and the rest
   of Contour does not notice.
2. **A script interpreter compiled to WASM** (QuickJS-ng or Lua; §13). Justification: it is what
   makes the "scripting" in #398 true for users without a compiler toolchain. Mitigation: it is a
   *guest*, not a host dependency — a prebuilt `.wasm` artifact with no C++ link-time coupling, so it
   can be dropped, swapped, or shipped out-of-band without touching the host.

---

## 9. Testing strategy

[AGENT.md](../../AGENT.md) requires every code area to be testable and new code to land with tests.
For an extension host the interesting failures are all at boundaries, so the tests are too.

**`vtscript_test`** (Catch2, Qt-free, like `vthost`'s suites):

- **ABI codec golden tests**, in the style of `Pdu_test`: every record and variant round-trips;
  malformed payloads decode to an error, not to a plausible small value; out-of-range integers are
  rejected rather than narrowed. This is the layer where a bug is a security bug.
- **A mock host** implementing every interface over fake state, so guest behaviour is testable
  without a terminal.
- **Fixture guests, one per tier** — a minimal compiled module and a minimal script — exercising
  every host call. Built once and committed, so the suite does not need a WASM toolchain to run.
- **Permission-denial tests** for every capability: denied is `permission-denied`, never a silent
  empty result. Cheap to write, and exactly the class of bug that turns into a CVE.
- **Limit and watchdog tests**: infinite loop is interrupted; memory hog traps; overrunning callback
  is suspended and its declarations dropped; a poisoned instance's declarations are withdrawn before
  its memory is freed.
- **Lifecycle tests**: unload during event delivery, unload with declarations live, session close
  with an activation in flight.

**`contour_gui_test`** (offscreen) for the client side: an extension's status-line item renders; its
overlay declarations reach the render buffer; its palette rows appear; its panel model drives the
QML. Remembering that TSM/`TerminalSession`/`TerminalDisplay` are not headless-constructible, these
go through the existing offscreen harness, and anything genuinely display-gated is tagged
`[display]`.

**Benchmark gate** (`termbench-pro`): a build with the host compiled in and no extension loaded shows
no throughput regression against a build without it. §2.3's claim, mechanized.

**`contour extension test`**: runs an extension's own assertions against a headless terminal, so
third-party extensions get CI. This is also how we discover our own API's rough edges — the first
extension we did not write is the real test of §7.

---

## 10. Distribution and tooling

CLI verbs under the existing `crispy::cli` tree in `ContourApp.cpp`:

```
contour extension list [--verbose]           # installed, version, tier, permissions, API range
contour extension install <id|path|url>      # verify signature, show permissions, prompt, install
contour extension remove <id>
contour extension permissions <id>           # show; --grant/--revoke <perm>
contour extension dev <dir> [--grant ...]    # load unsigned, watch, hot reload
contour extension test <dir>                 # run its assertions headlessly
contour extension package <dir>              # produce a signed bundle
contour extension search <query>
```

**Registry**: a git-backed index of manifests plus artifact URLs and hashes — Zed's model, which
avoids running a service and makes the index reviewable through pull requests. The index entry
carries the permission set, so a reviewer sees a widening in the diff.

**Guest SDKs**: a Rust crate and a C header generated from the IDL; for the script tier, a typings
stub so editors complete the API. All generated, per §2.6 — an SDK that drifts from the host is worse
than no SDK.

---

## 11. Phasing

Each milestone ships at least one of §1.3's use cases end to end. Nothing is promoted out of
experimental until a third-party extension has used it.

**M1 — Read-only observer.** The host (`src/vtscript`), both tiers, manifest, permission grant
store, activation events, the subscription-mask mechanism and its benchmark gate. Interfaces:
`grid` (reads), `shell`, `ui.statusline`, `ui.palette`, `host.store`/`log`/`time`. **No write
capability anywhere in the system** — which keeps the first milestone's security surface small enough
to actually review. Ships: git awareness (#4), and most of the block navigator's information display.

**M2 — Contribution.** `actions`, `input` (claims and clipboard transforms), `ui.overlay`,
`ui.hints`, `ui.notify`, `host.fs`, `host.proc`. Ships: block navigator (#1), error triage (#2),
secret redaction (#3).

**M3 — Injection & protocol.** `vt` (private APC/OSC namespace, both taps), the confirmed-injection
flow, `workspace` mutation, `host.net`, `host.secrets`, `host.clipboard`. Ships: recorder (#5),
symbol picker (#7), agent bridge (#9), collaboration (#10).

**M4 — Providers.** `pty`, `ui.panel`, `ui.text`, `ui.render`. Ships: transports (#6), visual
effects (#8).

Cross-cutting, tracked from M1: the Component Model migration (§8.3), which is a codec swap behind
the generated ABI and should stay a codec swap.

---

## 12. Alternatives considered

**Lua, embedded natively (wezterm).** Excellent ergonomics, a tiny dependency, and a proven fit for
terminal configuration. Rejected as the *foundation* because a natively-embedded interpreter has
ambient access to the host process: no memory isolation, no CPU limit that is not cooperative, and a
crash in a plugin is a crash in the terminal. #398's crash-isolation requirement is not satisfiable
this way. Note that the WASM design **subsumes** this: a Lua interpreter compiled to `wasm32` is
tier 2, with the ergonomics and without the ambient authority.

**Python, embedded natively (kitty).** Same objection, amplified — a large runtime, a system
dependency with its own version politics, and a plugin ecosystem that can `import os`. kitty's
kittens are genuinely useful, and the model is that plugins are trusted. We do not want to require
that.

**JavaScript via a natively-linked QuickJS.** The best of the native options: small, fast, easy to
sandbox at the language level. Rejected for the same reason — language-level sandboxing is a much
weaker claim than linear-memory isolation plus fuel, and the moment an extension wants a native
module the boundary is gone.

**Native `dlopen` plugins.** Maximum performance and zero marshalling. Rejected outright: no
isolation of any kind, an ABI that breaks on every compiler and standard-library change, per-platform
build matrices for every extension author, and a crash surface that includes memory corruption of
terminal state. The performance argument is also weaker than it looks, because §2.2 already forbids
the hot-path hooks that would need it.

**Out-of-process only (iTerm2's WebSocket API).** Any language, no toolchain, perfect crash
isolation, and it makes automation-shaped extensions trivial. Rejected as the *only* mechanism
because latency rules out overlays, render slots, and input claims — the interfaces that make
extensions feel like part of the terminal rather than a script poking it. Kept as a future tier
(§3.2) precisely because it is excellent for the automation-shaped half.

**No extension API at all (Ghostty's position).** A defensible product stance: every extension point
is a compatibility commitment and a support surface, and a terminal that does one thing well is a
real thing to want. Rejected because §1.2 changes the arithmetic — most of the extension points
already exist as registries, so the marginal cost here is the sandbox and the security model, not an
architecture. And because the block navigator, transport provider, and redaction use cases are all
things users want that we should not each ship ourselves.

---

## 13. Open questions

1. **Which script interpreter?** QuickJS-ng gives the larger author pool and better editor tooling;
   Lua gives a much smaller module and a precedent among terminal users. Decide on measured module
   size and cold-start, not taste.
2. **The reserved OSC number.** §4.1 proposes `OSC 889` as the OSC-shaped fallback (Contour already
   holds 888). Needs a check against the community's de-facto registry before it is documented, and a
   decision on whether to register it publicly.
3. **Does `ui.render` ship at all?** It is the most-requested item in #398 and the least stable to
   build against while the RHI renderer is still moving. Deferring it to M4 is the current answer;
   dropping it in favour of a smaller "background layer only" slot is a live option.
4. **Overlays under BiDi.** Decorations are declared in logical order; cells go to visual order at
   `RenderBufferBuilder`. A logically-contiguous range in an RTL run is not visually contiguous.
   Does the host split a declaration into visual runs (correct, more work), or does it refuse to
   decorate bidi lines in M2 (honest, limited)?
5. **Trust tier for `terminal:inject-input`.** §6.5 restricts non-interactive injection to a verified
   tier. Is a verified tier something this project can actually staff? If not, the confirmed-injection
   flow is the *only* path, for everyone — which is arguably the better answer anyway.
6. **Component Model timing.** Betting on the C API landing component support makes the migration
   free; betting wrong means maintaining a hand-rolled codec indefinitely. The generated-ABI hedge
   makes either outcome survivable, but the bet should be revisited each Wasmtime major.
7. **Per-profile extension sets.** Should a profile be able to enable a subset of extensions, the way
   it selects fonts and colours? Attractive for "my work profile has the k8s extension", and it
   interacts with activation events in ways worth thinking through before, not after.
8. **Config defaults for extension settings.** Contour's own lesson applies: a changed default never
   reaches an existing user, so extension settings need the same `builtinFallback…` treatment —
   consulted *after* the user's file — rather than a default baked into the manifest.

---

## 14. Appendix: interface signatures

WIT-shaped, illustrative rather than final; the point is the shape and the granularity, not the
spelling. Permissions are noted per function. Common types first.

### 14.1 Common types

```wit
package contour:terminal@0.1.0;

interface types {
  /// A 0-based grid position. Negative `line` addresses scrollback.
  record cell-location { line: s32, column: u16 }

  /// A half-open range of lines. `logical` distinguishes reflow-invariant
  /// logical lines from on-screen physical rows — never inferred.
  record line-range { first: s32, count: u32, logical: bool }

  record cell-range { start: cell-location, end-exclusive: cell-location, logical: bool }

  record page-size { lines: u32, columns: u16 }

  variant color { default-fg, default-bg, indexed(u8), rgb(tuple<u8, u8, u8>) }

  flags cell-flags {
    bold, faint, italic, underline, double-underline, curly-underline,
    dotted-underline, dashed-underline, blinking, rapid-blinking, inverse,
    hidden, crossed-out, overline, protected, character-protected,
  }

  record cell {
    text: string,                 // one grapheme cluster
    width: u8,
    fg: color, bg: color, underline-color: option<color>,
    flags: cell-flags,
    hyperlink: option<u32>,       // id into the line's hyperlink table
    image-covered: bool,
  }

  record line {
    stable-id: u64,               // survives scrolling; see Grid's stable row ids
    offset: s32,
    wrapped: bool,
    cells: list<cell>,
  }

  /// Opaque identities. Never an index — indices shift under a close.
  type window-id = u64;
  type tab-id = u64;
  type pane-id = u64;
  type session-id = u64;

  variant error {
    permission-denied(string),
    not-found,
    invalid-argument(string),
    unsupported,            // this build/placement does not offer it
    resync-required,        // grid identity was rebuilt; re-snapshot
    unavailable(string),    // transient
  }
}
```

### 14.2 `workspace`

```wit
interface workspace {
  use types.{window-id, tab-id, pane-id, session-id, page-size, error};

  record pane-info {
    id: pane-id, session: session-id, tab: tab-id,
    grid: page-size, focused: bool, zoomed: bool, title: string,
  }
  record tab-info {
    id: tab-id, window: window-id, title: string, raw-title: string,
    active: bool, panes: list<pane-id>, color: option<tuple<u8,u8,u8>>,
  }
  enum direction { left, right, up, down }
  enum orientation { horizontal, vertical }

  // --- workspace:read -----------------------------------------------------
  windows:      func() -> result<list<window-id>, error>;
  tabs:         func(window: window-id) -> result<list<tab-info>, error>;
  panes:        func(tab: tab-id) -> result<list<pane-info>, error>;
  active-pane:  func() -> result<pane-info, error>;
  pane:         func(id: pane-id) -> result<pane-info, error>;

  // --- workspace:write ----------------------------------------------------
  create-tab:   func(window: window-id, profile: option<string>) -> result<tab-id, error>;
  close-tab:    func(tab: tab-id) -> result<_, error>;
  activate-tab: func(tab: tab-id) -> result<_, error>;
  move-tab:     func(tab: tab-id, to-index: u32) -> result<_, error>;
  move-tab-to-window: func(tab: tab-id, to: window-id, to-index: u32) -> result<_, error>;
  set-tab-title: func(tab: tab-id, title: option<string>) -> result<_, error>;
  set-tab-color: func(tab: tab-id, color: option<tuple<u8,u8,u8>>) -> result<_, error>;

  split-pane:   func(pane: pane-id, how: orientation, profile: option<string>)
                  -> result<pane-id, error>;
  close-pane:   func(pane: pane-id) -> result<_, error>;
  focus-pane:   func(pane: pane-id) -> result<_, error>;
  focus-direction: func(tab: tab-id, dir: direction) -> result<_, error>;
  move-pane:    func(pane: pane-id, dir: direction) -> result<_, error>;
  swap-pane:    func(pane: pane-id, dir: direction) -> result<_, error>;
  set-pane-ratio: func(split: pane-id, ratio: f64) -> result<_, error>;
  toggle-zoom:  func(pane: pane-id) -> result<_, error>;

  apply-layout: func(window: window-id, layout: string) -> result<_, error>;
  capture-layout: func(window: window-id) -> result<string, error>;

  // --- events (coalesced) -------------------------------------------------
  record change { windows: bool, tabs: bool, panes: bool, focus: bool }
  on-change: func(what: change);
}
```

### 14.3 `grid`

```wit
interface grid {
  use types.{line, line-range, cell-range, cell-location, pane-id, page-size, error};

  enum text-form { plain, with-sgr, html, vt }
  record hyperlink { id: u32, uri: string }
  record snapshot { lines: list<line>, hyperlinks: list<hyperlink>, cursor: cell-location }

  // --- grid:read ----------------------------------------------------------
  size:          func(pane: pane-id) -> result<page-size, error>;
  history-lines: func(pane: pane-id) -> result<u32, error>;
  read-text:     func(pane: pane-id, range: line-range, form: text-form) -> result<string, error>;
  read-cells:    func(pane: pane-id, range: line-range) -> result<snapshot, error>;
  cursor:        func(pane: pane-id) -> result<cell-location, error>;

  selection:      func(pane: pane-id) -> result<option<cell-range>, error>;
  selection-text: func(pane: pane-id) -> result<string, error>;
  set-selection:  func(pane: pane-id, range: option<cell-range>) -> result<_, error>;

  find:      func(pane: pane-id, needle: string, from: cell-location, backward: bool)
               -> result<option<cell-location>, error>;

  /// Follow changes. Opaque cursor; `resync-required` means re-snapshot.
  /// Coalesced at frame granularity — this cannot be asked to fire per keystroke.
  resource delta-cursor {
    constructor(pane: pane-id);
    next: func() -> result<list<line>, error>;
  }
  on-changed: func(pane: pane-id);      // "there is something to drain"

  // --- grid:write (high; prefer ui/overlay) -------------------------------
  write-text: func(pane: pane-id, text: string) -> result<_, error>;
}
```

### 14.4 `shell`

```wit
interface shell {
  use types.{pane-id, line-range, error};

  record block {
    id: u64,
    pane: pane-id,
    command: option<string>,        // present only if the shell sent OSC 133;C cmdline
    exit-code: option<s32>,         // absent while running
    started-at-ms: u64,
    duration-ms: option<u64>,
    cwd: option<string>,
    prompt-range: line-range,       // logical
    output-range: line-range,       // logical
  }
  enum block-part { prompt, command, output, all }

  // --- shell:observe ------------------------------------------------------
  on-prompt-start:   func(pane: pane-id, clickable: bool);
  on-prompt-end:     func(pane: pane-id);
  on-command-start:  func(pane: pane-id, command: option<string>);
  on-command-finish: func(b: block);
  on-cwd-changed:    func(pane: pane-id, cwd: string);

  blocks:      func(pane: pane-id, last-n: u32) -> result<list<block>, error>;
  block-at:    func(pane: pane-id, line: s32) -> result<option<block>, error>;
  block-text:  func(id: u64, part: block-part) -> result<string, error>;

  /// Where the live prompt is — or why there isn't one (an application is up).
  record live-prompt { range: line-range, cursor-column: u16 }
  live-prompt-span: func(pane: pane-id) -> result<live-prompt, error>;

  goto-block: func(pane: pane-id, from: u64, forward: bool) -> result<option<u64>, error>;
}
```

### 14.5 `vt`

```wit
interface vt {
  use types.{pane-id, error};

  // --- vt:claim -----------------------------------------------------------
  /// Messages addressed to this extension arrive here, whether they came in as
  /// `APC x <ext-id> ; payload ST` or `OSC 889 ; <ext-id> ; payload ST`.
  record message { pane: pane-id, payload: list<u8>, correlation: option<string> }
  on-message: func(msg: message);
  reply: func(pane: pane-id, correlation: string, payload: list<u8>) -> result<_, error>;

  // --- vt:tap-sequences (read-only) --------------------------------------
  record sequence {
    category: string,          // "CSI" | "OSC" | "DCS" | "ESC" | "C0"
    mnemonic: option<string>,  // resolved from the Functions.hpp registry
    raw: string,
    handled: bool,
  }
  on-sequence: func(pane: pane-id, seq: sequence);

  // --- vt:tap-bytes (high: sees everything typed) ------------------------
  on-bytes-out: func(pane: pane-id, bytes: list<u8>);   // PTY → terminal
  on-bytes-in:  func(pane: pane-id, bytes: list<u8>);   // terminal → PTY
}
```

### 14.6 `pty` (the guest implements this)

```wit
interface pty-provider {
  use types.{page-size, error};

  record open-request { url: string, size: page-size, env: list<tuple<string, string>> }

  resource connection {
    /// Resolves when bytes are available; empty list means EOF.
    read: func() -> result<list<u8>, error>;
    write: func(bytes: list<u8>) -> result<u32, error>;
    resize: func(size: page-size) -> result<_, error>;
    close: func();
  }

  /// Called by the host when a session for a claimed scheme is opened.
  open: func(req: open-request) -> result<connection, error>;
  schemes: func() -> list<string>;
}
```

### 14.7 `actions` and `input`

```wit
interface actions {
  use types.{pane-id, error};

  /// Declared in the manifest; this is only the invocation callback.
  record invocation { id: string, args: list<tuple<string, string>>, pane: option<pane-id> }
  on-invoke: func(inv: invocation);

  /// Enable/disable a declared action (greys it out in palette and menus).
  set-enabled: func(id: string, enabled: bool) -> result<_, error>;
}

interface input {
  use types.{pane-id, cell-location, error};

  flags modifiers { shift, alt, control, super, caps-lock, num-lock }
  record key-event { key: string, text: string, mods: modifiers, pane: option<pane-id> }
  record mouse-event { button: u8, mods: modifiers, at: cell-location, pane: pane-id }

  // --- input:claim --------------------------------------------------------
  on-key:   func(ev: key-event) -> bool;      // true = consumed
  on-mouse: func(ev: mouse-event) -> bool;

  /// A modal consumer, peer to Vi mode. One at a time; the status line names it.
  enter-mode: func(name: string) -> result<_, error>;
  leave-mode: func() -> result<_, error>;

  // --- input:transform-clipboard -----------------------------------------
  on-copy:  func(text: string) -> string;
  on-paste: func(text: string) -> string;

  // --- terminal:inject-input (crown jewel, §6.2) -------------------------
  /// The extension proposes; the user sees the exact bytes and confirms.
  /// The extension is not told how the user reacted, only the outcome.
  propose-input: func(pane: pane-id, text: string, rationale: string) -> result<bool, error>;

  /// Direct injection. Verified tier only, and still user-granted.
  send-input: func(pane: pane-id, text: string) -> result<_, error>;
}
```

### 14.8 `ui/overlay`, `ui/statusline`, `ui/panel`, `ui/palette`, `ui/hints`

```wit
package contour:ui@0.1.0;

interface overlay {
  use contour:terminal/types.{cell-range, line-range, color, pane-id, error};

  variant decoration {
    underline(tuple<underline-style, option<color>>),
    strike-through(option<color>),
    box(option<color>),
    background(color),
    foreground(color),
    dim(f32),
  }
  enum underline-style { single, double, curly, dotted, dashed }

  record range-decoration { range: cell-range, what: decoration, priority: u8 }
  record gutter-mark { line: s32, glyph: string, color: option<color> }
  record badge { line: s32, text: string, fg: option<color>, bg: option<color> }
  record fold { range: line-range, collapsed: bool, summary: string }
  record region-tint { range: line-range, color: color, alpha: f32 }

  /// One atomic submission REPLACES this extension's whole overlay for the pane.
  /// Diffing is the host's job; an extension that has nothing to say submits nothing
  /// and costs nothing.
  record overlay-set {
    ranges: list<range-decoration>,
    gutter: list<gutter-mark>,
    badges: list<badge>,
    folds: list<fold>,
    tints: list<region-tint>,
  }
  submit: func(pane: pane-id, set: overlay-set) -> result<_, error>;
  clear:  func(pane: pane-id) -> result<_, error>;

  /// The host tells the extension what is visible, so it decorates only that.
  on-viewport-changed: func(pane: pane-id, visible: line-range);
  on-fold-toggled:     func(pane: pane-id, line: s32, collapsed: bool);
}

interface statusline {
  use contour:terminal/types.{error};
  record item { slot: string, text: string, style: option<string>, tooltip: option<string> }
  set: func(i: item) -> result<_, error>;
  clear: func(slot: string) -> result<_, error>;
}

interface panel {
  use contour:terminal/types.{error};

  variant widget {
    text(string),
    markdown(string),
    input(input-spec),
    list(list-spec),
    table(list<tuple<string, string>>),
    progress(f32),
    image(list<u8>),
    row(list<widget>),
    column(list<widget>),
  }
  record input-spec { id: string, placeholder: string, value: string, secret: bool }
  record list-item { id: string, title: string, detail: option<string>, icon: option<string> }
  record list-spec { id: string, items: list<list-item>, filterable: bool, selected: option<string> }

  enum slot { quick-pick, side-panel, popover }
  record panel-spec { slot: slot, title: string, body: widget }

  /// The host owns placement and dispatch — an extension cannot reintroduce the
  /// unclamped-popup or reentrant-onClosed traps.
  open:   func(spec: panel-spec) -> result<u64, error>;
  update: func(id: u64, body: widget) -> result<_, error>;
  close:  func(id: u64) -> result<_, error>;

  on-selected:  func(panel: u64, list-id: string, item-id: string);
  on-submitted: func(panel: u64, input-id: string, value: string);
  on-closed:    func(panel: u64);
}

interface palette {
  record row { id: string, title: string, detail: option<string>, group: option<string> }
  /// Asked each time the palette opens, so rows may depend on live state.
  provide: func(query: string) -> list<row>;
  on-chosen: func(id: string);
}

interface hints {
  use contour:terminal/types.{pane-id, cell-range};
  record pattern { id: string, regex: string, title: string }
  patterns: func() -> list<pattern>;
  on-hint-activated: func(pattern-id: string, matched: string, pane: pane-id, at: cell-range);
}
```

### 14.9 `ui/notify`, `ui/render`, `ui/text`

```wit
interface notify {
  use contour:terminal/types.{error};

  enum urgency { low, normal, critical }
  record notification {
    identifier: string,        // reused to REPLACE an earlier one, as OSC 99 does
    title: string, body: string,
    urgency: urgency,
    actions: list<tuple<string, string>>,   // (id, label)
  }
  post:    func(n: notification) -> result<_, error>;
  discard: func(identifier: string) -> result<_, error>;

  on-activated: func(identifier: string, action-id: option<string>);
  on-closed:    func(identifier: string);
}

interface render {
  use contour:terminal/types.{pane-id, error};

  /// Slots are fixed by the host; draw ORDER is the only z-ordering the renderer
  /// has, so extensions choose a slot, never a position in the pass list.
  enum slot { background, cursor-decoration, pane-treatment, post-process }

  /// Uniform values only — no buffers, no draw calls, no other pane's texture.
  /// Geometry reaches the shader in CELLS and normalized coordinates, never in
  /// device pixels: a DPR or font-size change must not need extension action.
  variant uniform-value { f(f32), v2(tuple<f32,f32>), v3(tuple<f32,f32,f32>),
                          v4(tuple<f32,f32,f32,f32>), i(s32), b(bool) }

  record effect {
    slot: slot,
    shader: string,                                  // host validates and compiles
    uniforms: list<tuple<string, uniform-value>>,    // names declared in the manifest
    animated: bool,                                  // opts into per-frame updates
  }
  install: func(pane: option<pane-id>, e: effect) -> result<u64, error>;
  set-uniforms: func(id: u64, uniforms: list<tuple<string, uniform-value>>) -> result<_, error>;
  remove: func(id: u64) -> result<_, error>;

  /// Only delivered to an effect that declared `animated`, and coalesced to the
  /// display's own frame cadence — an extension cannot request a faster one.
  on-frame: func(id: u64, elapsed-ms: u64);
}

interface text-provider {
  use contour:terminal/types.{error};

  /// Consulted only after the font stack missed — last in the chain, so an
  /// extension can neither slow down nor alter ordinary text.
  record glyph-request { cluster: string, cell-width: u16, px-size: u32 }
  variant glyph {
    bitmap(tuple<u32, u32, list<u8>>),   // width, height, RGBA
    svg(string),
    none,                                 // decline; the next provider is asked
  }
  provide-glyph: func(req: glyph-request) -> glyph;
}
```

### 14.10 `host` services

```wit
package contour:host@0.1.0;

/// Preopened roots only. A path is resolved WITHIN a handle the extension was
/// given; there is no syntax by which it can name a directory it was not handed,
/// and no current-directory to be relative to.
interface fs {
  use contour:terminal/types.{error};

  resource dir {
    /// Names one of the roots granted in the manifest: "data" (the extension's
    /// own directory) or a user-granted label.
    open: static func(root: string) -> result<dir, error>;
    read-file:  func(path: string) -> result<list<u8>, error>;
    write-file: func(path: string, contents: list<u8>) -> result<_, error>;
    remove:     func(path: string) -> result<_, error>;
    list:       func(path: string) -> result<list<string>, error>;
    exists:     func(path: string) -> bool;
    /// Sub-directory handle, so authority can be attenuated but never widened.
    open-dir:   func(path: string) -> result<dir, error>;
  }
}

interface log {
  enum level { trace, debug, info, warning, error }
  /// Lands in the extension's own logstore category: `contour --debug ext.<id>`.
  write: func(lvl: level, message: string);
}

interface store {
  use contour:terminal/types.{error};
  get:    func(key: string) -> option<list<u8>>;
  set:    func(key: string, value: list<u8>) -> result<_, error>;
  remove: func(key: string);
  keys:   func(prefix: string) -> list<string>;

  /// The extension's own config section, schema declared in the manifest and
  /// rendered in the GUI settings page.
  config: func() -> string;                  // resolved values as JSON
  on-config-changed: func(values: string);
}

interface time {
  now-ms: func() -> u64;
  monotonic-ms: func() -> u64;
  record timer-spec { after-ms: u64, repeat: bool }
  set-timer: func(spec: timer-spec) -> u64;   // host enforces a minimum interval
  cancel-timer: func(id: u64);
  on-timer: func(id: u64);
}

interface proc {
  use contour:terminal/types.{error, pane-id};
  record spawn-spec {
    program: string,                          // must match the manifest allowlist
    args: list<string>,
    cwd: option<string>,
    env: list<tuple<string, string>>,
    stdin: option<list<u8>>,
  }
  record output { exit-code: s32, stdout: list<u8>, stderr: list<u8> }
  /// Captured child. Bounded output, bounded runtime.
  run: func(spec: spawn-spec) -> result<output, error>;
  /// PTY-backed child that becomes a visible pane the user can see and kill.
  run-in-pane: func(spec: spawn-spec, tab: option<u64>) -> result<pane-id, error>;
}

interface net {
  use contour:terminal/types.{error};
  record connect-spec { host: string, port: u16, tls: bool }
  resource stream {
    read:  func(max: u32) -> result<list<u8>, error>;
    write: func(bytes: list<u8>) -> result<u32, error>;
    close: func();
  }
  connect: func(spec: connect-spec) -> result<stream, error>;
  resource listener {
    accept: func() -> result<stream, error>;
    close: func();
  }
  listen: func(port: u16) -> result<listener, error>;
}

interface secrets {
  use contour:terminal/types.{error};
  get:    func(key: string) -> result<option<string>, error>;
  set:    func(key: string, value: string) -> result<_, error>;
  remove: func(key: string) -> result<_, error>;
}

interface clipboard {
  use contour:terminal/types.{error};
  enum which { clipboard, primary }
  read:  func(w: which) -> result<string, error>;
  write: func(w: which, text: string) -> result<_, error>;
}
```

### 14.11 Worked example: block navigator (compiled tier)

Manifest as in §5.1. The guest, in outline:

```rust
use contour_ext::{shell, ui::overlay, actions, store};

struct State { threshold: u32, folded: Vec<u64> }

// M1: information only — no writes, no injection.
fn on_command_finish(b: shell::Block, st: &mut State) {
    let mut set = overlay::OverlaySet::default();

    if let Some(code) = b.exit_code {
        set.gutter.push(overlay::GutterMark {
            line: b.output_range.first,
            glyph: if code == 0 { "▏".into() } else { "▍".into() },
            color: Some(if code == 0 { GREEN } else { RED }),
        });
        if code != 0 {
            set.badges.push(overlay::Badge {
                line: b.prompt_range.first,
                text: format!("exit {code}"), fg: Some(RED), bg: None,
            });
            set.tints.push(overlay::RegionTint {
                range: b.output_range, color: RED, alpha: 0.06,
            });
        }
    }
    if let Some(ms) = b.duration_ms.filter(|ms| *ms > 1000) {
        set.badges.push(overlay::Badge {
            line: b.prompt_range.first,
            text: format!("{:.1}s", ms as f64 / 1000.0), fg: Some(DIM), bg: None,
        });
    }
    if b.output_range.count > st.threshold {
        set.folds.push(overlay::Fold {
            range: b.output_range, collapsed: false,
            summary: format!("{} lines", b.output_range.count),
        });
    }
    overlay::submit(b.pane, set).ok();   // one atomic replace; host diffs
}

fn on_invoke(inv: actions::Invocation, st: &mut State) {
    match inv.id.as_str() {
        "org.example.next-block" => { /* shell::goto_block(.., forward = true) */ }
        "org.example.fold-block" => { /* toggle in st.folded, resubmit overlay */ }
        _ => {}
    }
}
```

Note what is absent: no per-cell callback, no grid write, no draw call, no thread. The extension is
told a command finished and answers with data.

### 14.12 Worked example: git status line (script tier)

`~/.config/contour/extensions/org.example.gitline/main.js`, with a manifest requesting
`shell:observe`, `ui:statusline`, `proc:spawn` (`git`):

```js
import { shell, statusline, proc, time } from "contour";

let cwd = null;

shell.onCwdChanged((pane, dir) => { cwd = dir; refresh(); });
shell.onCommandFinish(() => refresh());          // a commit changes the answer
time.setTimer({ afterMs: 15000, repeat: true }); // and so does a background fetch
time.onTimer(refresh);

async function refresh() {
  if (!cwd) return statusline.clear("git");
  const r = await proc.run({ program: "git", args: ["-C", cwd, "status", "-sb", "--porcelain=v2"] });
  if (r.exitCode !== 0) return statusline.clear("git");     // not a repo: say nothing
  const { branch, dirty, ahead, behind } = parse(r.stdout);
  statusline.set({
    slot: "git",
    text: `${branch}${dirty ? "*" : ""}${ahead ? ` ↑${ahead}` : ""}${behind ? ` ↓${behind}` : ""}`,
    style: dirty ? "Bold,Color=Yellow" : "Color=Green",
  });
}
```

Forty lines, no toolchain, and the same sandbox as the compiled tier: it can run `git` because the
manifest said `git` and the user agreed, and it can do nothing else.

### 14.13 Worked example: `ssh://` transport provider (compiled tier)

```rust
use contour_ext::{pty_provider as pp, host::{net, secrets}};

fn schemes() -> Vec<String> { vec!["ssh".into()] }

fn open(req: pp::OpenRequest) -> Result<pp::Connection, Error> {
    let target = parse_ssh_url(&req.url)?;                    // user@host:port/…
    let key = secrets::get(&format!("ssh/{}", target.host))?;  // never in the manifest
    let sock = net::connect(net::ConnectSpec {
        host: target.host.clone(), port: target.port, tls: false,
    })?;
    let chan = ssh_handshake(sock, &target, key)?;
    chan.request_pty(req.size)?;                              // honouring size is mandatory
    Ok(chan.into_connection())
}
```

Because the session lives in the daemon, this connection survives the GUI restarting, and a
`contour client` attaching from another machine sees the same session. That is the payoff of the
session/client split in §3.1: an extension inherits persistence it did not have to implement.
