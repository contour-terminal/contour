# Orchestrator mouse and url-open: implementation plan (Fleet + Focus, phase 6)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** `o` opens the selected run's link in the browser of the machine the user sits at, and the dashboard
answers the mouse: a click selects a row, switches a Focus tab, answers with a chip, focuses the message box,
runs a palette entry or highlights a launcher candidate; a double click runs the primary action; the wheel
scrolls the pane under the pointer; a click on a link opens it.

**Architecture:**
- **What a served tab drops.** A served tab executes only drawing sequences
  (`vtbackend/vt/ServedTabSequences.hpp`): OSC 52 and OSC 8 written through `tab-write` never reach the
  terminal. That policy stays. The OSC 52 copy behind `o` is dead, so it goes. The OSC 8 framing stays: the
  spec makes every link a hyperlink, and the frame's hyperlink regions are what a click on a link is
  hit-tested against.
- **`url-open` (new daemon-world import).**
  `url-open: func(url: string, origin: origin) -> result<_, host-error>`.
  - Manifest line `url-open` (since 2, no words); consent line "It may ask your terminal to open web links
    you choose". Without it the call is `not-granted`.
  - Policy (`vthost/UrlToOpen.hpp`, one constexpr rule both ends apply): `http://` or `https://` with a
    host, at most 2 KiB, no control character or blank; else `bad-argument`.
  - Routing (`ServedSessions::openUrl`): the window of the origin's action (claimed once, as a reveal is),
    else the window a key or click into one of the extension's served tabs came from within 2 s; none is
    `gone`. That window's connection must have negotiated `Capability::UrlOpen`, else `not-supported-yet`
    (the connection mirror publishes it with the selection). The loop then sends the new PDU `OpenUrl` to
    that connection only.
  - Frontend: `NativeClient` hands `OpenUrl` on as an extension event; `NativeController` queues it;
    `DaemonLink` checks it again with the same rule and opens it through the app's `ExternalLauncher`, the
    path a ctrl+click on a hyperlink takes (`TerminalSession::followHyperlink` → `openUrl`). No
    confirmation: a hyperlink click asks none either.
- **Guest.** `o` asks for an `OpenUrlAction`; the driver calls `url-open` with the dashboard's origin and
  reports on the bar: `opening #2369 on your machine`, or `not opened: <why>`.
- **Mouse.** Every frame sets DECSET 1000 and 1006 beside 2004 (both in `ServedTabModes`; no host change).
  core-cpp's `VtParser` already decodes SGR reports into `MouseEvent`s. `view/HitMap.{hpp,cpp}`: the
  regions the components record while they draw (`Hit{kind, value}` at screen rectangles; the last recorded
  wins, so a popup covers what lies beneath it), cleared before each draw. One table maps a hit kind to
  what a click on it does; links are looked up in the rendered buffer's hyperlink regions.

Spec: `docs/superpowers/specs/2026-11-22-orchestrator-fleet-focus-design.md`, "Links", "Mouse", "Errors".

## Global constraints

- No change to the served-tab sequence policy.
- Zero warnings, clang-tidy clean, `-j4`. Goldens rewritten only deliberately (the `o` label changes).

## File map

| File | Change |
| --- | --- |
| `wit/daemon/world.wit`, guest bindings | `url-open` |
| `src/vtextension/{DaemonImports,IDaemonHostServices,DaemonInstance,DaemonExtensionRunner,ServedTabService,Manifest,ExtensionConsent}` | the import, the manifest line, the consent line |
| `src/vthost/UrlToOpen.hpp` | the URL rule |
| `src/vthost/proto/{Negotiation,Pdu,PduTrace}`, `testing/PduExamples.hpp` | `Capability::UrlOpen`, PDU `OpenUrl` |
| `src/vthost/{ServedSessions,ConnectionWorkspaces,ExtensionHosting,DaemonWorldExtensions,SessionHost,NativeSession,Daemon}` | routing |
| `src/vthost/client/NativeClient` | `OpenUrl` as an extension event |
| `src/contour/remote/{NativeController,DaemonLink,DaemonLinkHost}`, `ContourGuiApp` | open it |
| `extensions/orchestrator/guest/*` | `urlOpen()`; `OpenUrlAction` |
| `extensions/orchestrator/view/Keys.*`, `Actions.cpp`, `Dashboard.*` | `o` opens; OSC 52 removed |
| `extensions/orchestrator/view/HitMap.*`, `Dashboard.cpp`, `Focus.cpp`, `Palette.cpp`, `Launcher.*` | the mouse |
| `src/vthost/AgentRun_test.cpp` | e2e: mouse bytes through the real guest |
| `README.md`, `docs/extensions.md`, `wit/daemon/README.md` | |

## Task 1: url-open in the host

- [ ] WIT, import row, instance handler, runner, served-tab service, fakes; manifest line and consent text.
- [ ] `UrlToOpen.hpp` with a table test; `Capability::UrlOpen`, PDU `OpenUrl` (round trip, trace).
- [ ] `ServedSessions::openUrl`: bad URL, no recent input, a window without the capability, delivery.
- [ ] Client and contour: event, queue, open through `ExternalLauncher`.

## Task 2: `o` opens the link

- [ ] Guest `urlOpen()`, `OpenUrlAction`, bar feedback; OSC 52 path and `Interaction::clipboard` removed.
- [ ] Manifest contract and consent regenerated; goldens for the label.

## Task 3: the mouse

- [ ] `HitMap` and its test; mouse modes in every frame.
- [ ] Regions: Fleet rows, Focus tabs, chips, the message box, palette entries, launcher candidates, the
      panes for the wheel; links from the buffer.
- [ ] Click, double click and wheel; tests per kind and an e2e through the real guest.

## Task 4: docs and simplify
