# Orchestrator quick replies: implementation plan (Fleet + Focus, phase 4)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** While an agent asks for a permission, the Focus shows its request in an asking box with the reply
chips `1 2 3`, and pressing a digit in the Fleet or the Focus answers Claude Code's menu, without opening the
agent's tab.

**Architecture:**
- **Host:** a new daemon-world import, `pane-key(pane, key)`, presses one key from a fixed set (`1`–`9`, Esc)
  in a pane the extension owns. It takes the path `pane-send` took (`DaemonImport`, `IPaneService`,
  `DeclaredPanes`, `IPaneHost::write`, `OwnedPanes`, `SessionHost`), so the same policy holds: an owned, open
  pane, and a manifest that declares `pane-input`. No new manifest line: `pane-input` now covers both imports,
  and the consent line says so.
- **Asking versus waiting:** the host knows why a program is blocked (OSC 7501 `kind`, set by the status hook
  from `notification_type` and `PermissionRequest`). The WIT `agent-status` gains `asking` (blocked, kind
  `permission`), appended so that the older cases keep their numbers; every other blocked kind stays
  `blocked`. In the orchestrator, `AgentStatus::Blocked` becomes `Waiting`, and `Asking` is new; a stored
  `blocked` reads as `waiting`.
- **Config:** `quick_replies`, a list of `{key, label, then}`; `key` a digit 1–9, `then` `wait` (default) or
  `message` (the message box opens for the reason). The defaults follow Claude Code's menu.
- **Engine:** `RequestDeclined{run}` turns an asking run `Waiting` and keeps it so while its pane still
  reports the same request, so that `3` leaves it waiting for the reason.
- **View:** `KeyCommand::Reply1`…`Reply9` bound to the digits; `Row::replies` carries the chips of an asking
  row; the actions table lists the replies for `Asking`, and the bar starts with `1 2 3 answer`.
  `BottomBox::Asking` draws the asking box. `Interaction::answered` fills the chip, dims the box, and says
  `still asks` after 10 s.
- **Guest:** `QuickReplyAction` → `IGuestHost::paneKey()`, refused with `<run> is no longer asking` when the
  run left asking meanwhile.

Spec: `docs/superpowers/specs/2026-11-22-orchestrator-fleet-focus-design.md`, "Quick replies".

## Global constraints

- The extension never reads the pane: the chip labels come from config, the request from the status hook.
- A digit answers only while the selected run asks (its request is then on screen in every band: the asking
  box, or the Needs-you row's second line); otherwise the bar says `select the request first`.
- `esc` never answers. Digits type in the message box, and an asking run has none.
- Zero warnings, clang-tidy clean, `-j4`. Goldens rewritten only deliberately.

## File map

| File | Change |
| --- | --- |
| `wit/daemon/world.wit` | `pane-key`, `pane-key-name`, `agent-status.asking` |
| `src/vtextension/{DaemonWorld,DaemonImports,DaemonInstance,DaemonExtensionRunner,IDaemonHostServices,PaneService,DeclaredPanes,ExtensionConsent}` | the import, its policy, the consent line |
| `src/vthost/{ExtensionHosting,OwnedPanes,DaemonWorldExtensions}` | `PaneStatus::kind`; `asking` from a permission block |
| `guest/{HostCalls.hpp,WitHost.cpp,Driver.*}`, `guest/bindings` | `paneKey()`, the status mapping, `QuickReplyAction` |
| `core/Run.*`, `core/Config.*`, `core/QuickReplies.*`, `core/Engine*` | the split, the config table, `RequestDeclined` |
| `view/{Keys,Actions,Fleet,Sections,Focus,Dashboard,Palette}` | the digits, the chips, the asking box |
| tests, goldens, `README.md`, `docs/extensions.md` | |

## Task 1: the host

- [ ] WIT: `pane-key`, `enum pane-key-name { digit-1 … digit-9, escape }`, `agent-status` + `asking`.
- [ ] `DeclaredPanes::press()`: `pane-input` required; bytes `1`…`9` or ESC through `IPaneHost::write`.
- [ ] vthost: `PaneStatus::kind`; `asking` for a permission block. Tests in vtextension_test (policy, ABI rows,
      consent) and vthost_test (a foreign pane is `not-owned`, the bytes reach the device).

## Task 2: asking versus waiting in the orchestrator

- [ ] `AgentStatus {Idle, Working, Done, Waiting, Error, Asking}`; persistence reads `blocked` as `waiting`.
- [ ] `rowStateOf`, `runLookOf`, the guest mapping; tests for each.

## Task 3: `quick_replies`

- [ ] `QuickReply`, defaults, validation (`NotAReplyKey`, `Duplicate`, `Empty`, `NotText`, `UnknownThen`).

## Task 4: the asking box and the digits

- [ ] `Reply1`…`Reply9`, `Row::replies`, the action rows, the bar chip, the palette labels.
- [ ] `BottomBox::Asking`; answered chip and dimming; the 10 s notice; `then: message` opens the message box.
- [ ] Driver: `QuickReplyAction` → `paneKey`, refusals; `RequestDeclined`.
- [ ] The primer mentions `1 2 3`.
- [ ] Goldens: the asking box at 120×40, 80×24 and 50×20; vthost e2e: a digit through the real guest.

## Task 5: docs and simplify
