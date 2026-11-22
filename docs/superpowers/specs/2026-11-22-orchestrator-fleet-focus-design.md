# Orchestrator: Fleet + Focus, one screen to work with agents

Date: 2026-11-22 · Branch: `feature/agent-orchestrator` · Follows
`2026-11-22-orchestrator-fleet-control-design.md`

## Problem

The dashboard grew one screen and one key per feature, and it now asks too much of its user:

- **Six places, each with its own keys:**
  - the dashboard;
  - the details, a separate page in the Narrow band;
  - the board screen;
  - the transcript screen;
  - the dispatch form;
  - the message line.

  `esc` and `enter` mean something different on each. There are about 25 keys to learn.
- **Three kinds of thing in one list:** PRs and issues you might start work on, live agents, and finished runs.
  The hint line does not change with the selection, so `b board` and `t transcript` are offered on an issue.
- **Nothing says where to look.** A blocked agent is a `▤` glyph in the AGENTS section, below two item lists.
- **Three ways to start work:** `d`, a task's letter key, and `n` for a topic.
- **Little use of the TUI library.** The view draws boxes on a `Canvas` and reads keys. It uses none of core-cpp's
  `CommandPalettePopup`, `FuzzyPickerPopup`, `Dialog`, `Tooltip`, `Spinner` or `MouseTracking`.

## Goal

Working with an agent means **seeing it, reading it and talking to it in one place**, with no screen to open and
nothing to memorise beyond `↑↓ ⏎ esc tab ^K n`. What the screen offers adapts to what is selected.

## Layout

```
╭─ Agents 7 ─────────────────────────╮ ╭─ pr-2375 · fix CI · sonnet ──────────── ● asking 2m ──╮
│ NEEDS YOU · 2                      │ │  Board   Transcript   Details                          │
│ ▶ pr-2375  fix CI          asking 2m│ │ ━━━━━━                                                 │
│   topic    flaky reflow   error    │ │ ## Fixing CI                                           │
│ WORKING · 2 OF 4                   │ │ ☑ reproduced on master                                 │
│   issue-2338  fix          ⠋ 12m   │ │ ☐ fix the reflow   ← now                               │
│   topic  reflow docs       ⠋  3m   │ │                                                        │
│ DONE · TO REVIEW                   │ │ ── latest ──────────────────────────────────────────── │
│   pr-2369  fix CI     PR updated   │ │ May I run `ctest -R reflow`?                           │
│ FINISHED                           │ │ [1 yes] [2 always] [3 no]                              │
│   pr-2366  completed      removed  │ │ ╭ message to pr-2375 ────────────────────────────────╮ │
│                                    │ │ │ █                                                  │ │
│ + new agent                        │ │ ╰────────────────────────────────────────────────────╯ │
╰────────────────────────────────────╯ ╰────────────────────────────────────────────────────────╯
 ⏎ reply · 1 2 3 answer · t transcript · e end · ^k all actions               ⬢ 3/4 · 2 need you
```

### Left pane: the Fleet, agents only

- The Fleet lists runs only, grouped by **what they need from you**, in this order:
  - **Needs you:** asking (a permission prompt), waiting for input, or stopped by an error;
  - **Working;**
  - **Done · to review:** the agent reported done, and the run is still open;
  - **Finished:** today's Finished section, folded to its newest five.

  Within a group the longest-waiting run comes first. A run moves between groups as its state changes, and the
  selection stays on the run, not on the position.
- Pull requests, issues and Jira issues are no longer listed on the dashboard. You browse and start them from the
  launcher (`n`), which opens on the items that need attention.
- `+ new agent` is the last Fleet row; `⏎` on it opens the launcher.

### Right pane: Focus, the selected run

The header shows the run, its task, its model and its state with its age.

There are three tabs, switched with `[` and `]` (or `^←`/`^→` while typing):

- **Board:** the rendered `STATUS.md`, re-read every five seconds, as the details panel shows it today.
- **Transcript:** follows the agent while it runs. It shows the last page, re-read every five seconds, and stays
  pinned to the end unless you scroll up. A `↓ new` marker brings you back.
- **Details:** what the details panel lists today: branch, worktree, URL, checks, kept reasons and failure words.

The Focus remembers the last tab per run kind:
- an asking agent opens on Transcript;
- a working agent opens on Board;
- a finished run opens on Details.

Under the tab, **latest** shows the last three transcript entries, unless the Transcript tab is open.

The **message box** is always present for a run whose pane is open. For a finished run it is replaced by that run's
actions (run again, dismiss).

### Bands

- **Wide (≥ 100 columns):** both panes side by side.
- **Medium (60–99):** Fleet above, Focus below at 60 % of the height; `tab` gives the Focus the whole body and back.
- **Narrow (< 60):** one pane at a time. `⏎`/`→` opens the Focus, and `esc` returns. That is the only depth.

## Focus model and keys

The user is in one of two places, shown by the highlighted border.

**Fleet (the default).** Keys select and act:

| Key | Does |
| --- | --- |
| `↑↓` `jk` `home` `end` | move |
| `⏎` | the selected row's **primary action** (see "Actions") |
| `tab` `→` | into the message box (Wide, Medium) or into the Focus (Narrow) |
| `[` `]` | switch the Focus tab without leaving the Fleet |
| `pgup` `pgdn` | scroll the Focus tab |
| `n` | the launcher |
| `/` | filter the rows |
| `^K`, `?` | the command palette |
| `1` `2` `3` | the quick replies, while the selected agent is asking |
| `t` `b` `e` `m` `d` `del` `z` `R` `I` `x` | today's shortcuts, kept; the palette and the action bar show them |

**Message box.** It is entered with `tab` or `→`, with `m`, with `⏎` when the primary action is *reply*, or by
starting to type `i`.

| Key | Does |
| --- | --- |
| printable keys | type |
| `⏎` | send |
| `alt+⏎` | new line; a paste keeps its line feeds |
| `esc` | back to the Fleet, keeping the draft (drafts are kept per run, in memory) |
| `^←` `^→` | switch the Focus tab |
| `pgup` `pgdn` | scroll it |
| `1` `2` `3` | quick replies, while the draft is empty and the agent is asking; otherwise they type |

A refused send keeps the draft and shows why on the action bar, as the message line does today.

## Actions: one table, adapted to the row

A data table drives the action bar, `⏎`, and the palette's ordering. It has one row per (row state, command):

```cpp
struct ActionRow {
    RowState when;          // what the selected row is
    KeyCommand command;     // what it does
    std::string_view key;   // its shortcut
    std::string_view label; // what the bar says
    uint8_t rank;           // 0 is the primary action: ⏎ does it, and the bar lists it first
};
```

`RowState` is one enum. The ranked actions for each state:

| RowState | Actions |
| --- | --- |
| `Asking` | `⏎ reply` · `1 2 3 answer` · `t transcript` · `e end` |
| `Waiting` | `⏎ reply` · `t transcript` · `b board` · `e end` |
| `Errored` | `⏎ transcript` · `m message` · `e end` · `d run again` |
| `Working` | `⏎ board` · `m message` · `t transcript` · `e end` |
| `Done` | `⏎ open PR` (or `transcript` without a PR) · `m follow up` · `e end` |
| `Finished` | `⏎ run again` · `del dismiss` · `t transcript` |
| `Kept` | `⏎ details` · `x retry removal` · `del dismiss` |
| `NewAgent` | `⏎ new agent` |
| `GroupHeader` | `⏎ fold` |

- The bar shows the actions that fit, in rank order, then `^k all actions`.
- An action that does not apply to a state has no row, so it is never offered.
- Adding a state or an action is one row.
- The existing `KeyRow` table stays the source of key → command. `ActionRow` only says which commands apply and in
  what order.

## The command palette (`^K`, `?`)

core-cpp's `CommandPalettePopup` lists every command:
- the selected row's actions first, from the table above;
- then the global ones: new agent, filter, refresh, install plugins, fold all, show the last 24 hours (the timeline, as a popup);
- each with its key.

Typing filters it fuzzily, and `⏎` runs the chosen command. It replaces the help overlay: `?` opens it too.
Commands that do not apply to the selection are listed dimmed, with the reason (for example, "e end · no run
selected").

## The launcher (`n` or `+ new agent`): browse and start

```
╭─ New agent ────────────────────────────────────────────╮
│ › flaky reflow█                                         │
│   ✎ topic "flaky reflow"                      ⏎ start   │
│   #2338 Flaky on Windows: SessionHost…   issue contour  │
│   #2375 Say why the stdio relay failed…  PR    contour  │
│─────────────────────────────────────────────────────────│
│ task fix ◂▸   model sonnet ◂▸   repo contour ◂▸         │
│ new branch agent/topic-flaky-reflow-… in ~/worktrees/…  │
╰─────────────────────────────────────────────────────────╯
```

It is one popup, built on `FuzzyPickerPopup` for the list. It replaces the item lists on today's dashboard,
`DispatchForm`, `d`, the task letter keys and `n`.

- **Empty query: browse.** The items are listed in three groups:
  - **Needs attention:** your PRs with failing CI or requested changes;
  - **My pull requests;**
  - **Issues without a PR** and Jira.

  Each group is ordered as today's lists are. CI, review and "an agent is on it" (`⬢`) are aligned glyph columns.
  One detail line describes the highlighted item: the failing job, the checks count, and the live agent's state.
  `o` opens it in the browser.

- **The query** filters the PRs, issues and Jira issues of every configured repository, matching their number and
  title. While the query is not empty, the first row is always **topic "<query>"**.
- **The settings line** shows the task, model and repository for the highlighted row:
  - the task is the first that applies to it;
  - the model is the task's;
  - the repository is the item's, or the selected run's for a topic.

  `tab` moves into the line, `←→` change a value, and `tab` returns to the list.
- **The preview line** shows the checkout (new branch or PR head) and the worktree. `alt+p` expands the full prompt
  and command line, as the dispatch form shows them today.
- **Setup warnings** keep the current rule: missing hooks, missing plugins, or the run limit reached. The warning
  is drawn on the settings line, and `⏎` asks again before it dispatches.
- Opened from an item, the query is the item's number and the item is highlighted.
- From a finished run, `run again` opens it with that run's item or topic, task and model.

## Quick replies

While the selected agent is **asking**, Claude Code shows its select menu (1 yes · 2 yes, and don't ask again ·
3 no). The Focus shows the replies as chips, and `1`, `2` or `3` answers.

**Asking versus waiting.** `AgentStatus::Blocked` splits into `Asking` and `Waiting`, from the status hook's
Notification `notification_type` (`permission_prompt` versus `idle_prompt`). Where the hook cannot tell, the run is
`Waiting` and shows no chips.

**Host: a key, not a paste.** A menu choice is a key press, not a message. `pane-send` wraps its text in a
bracketed paste and adds Enter, which a select menu does not take as a choice. So:

```wit
/// Presses one key from a fixed set in a pane the extension owns: a menu choice, never text.
pane-key: func(pane: string, key: pane-key-name) -> result<_, host-error>;
enum pane-key-name { digit-1, digit-2, digit-3, digit-4, digit-5, digit-6, digit-7, digit-8, digit-9, escape }
```

- **Policy.** The same checks as `pane-send` apply: an owned pane, from a template whose manifest declares
  `pane-input`. The consent line becomes "It may type text you write into those tabs, or press a number key you
  choose, when you send it from its own tab".
- **Guest.** The guest presses a key only on the user's `1`, `2` or `3`, while the run is asking. The labels come
  from a config table, `quick_replies:` (default yes, always, no, mapped to 1, 2, 3), so a changed Claude Code menu
  is fixed in config.
- **Live check before the host change.** In a real Claude Code pane, a raw `1` answers the prompt and a pasted `1`
  does not. If a paste does answer it, `pane-key` is dropped and `pane-send` gains a no-paste, no-Enter variant
  instead.

## Default permissions: an agent that knows what to do carries on

Most `Asking` states today are routine: build, test, read git state, and edit files in the worktree. An agent
should do those without waiting for a person who may be on another machine.

**How it reaches the agent.** The pane templates gain Claude Code's `--settings`, which takes a JSON string, through
a new argument `settings` of kind `text` (up to 8 KiB, which a profile fits easily):

```
pane agent       @worktrees = claude --model {model} --session-id {session} --settings {settings} --append-system-prompt {board} {prompt}
pane agent-plain @worktrees = claude --model {model} --session-id {session} --settings {settings} {prompt}
```

The engine builds the JSON from a **profile**: `{"permissions":{"defaultMode":…,"allow":[…],"deny":[…]}}`. It never
writes Claude Code's settings files.

**Profiles** are a data table. The built-in profiles below can be overridden or extended under `permissions:`:

```yaml
permissions:
  default: standard          # the profile a task without its own uses
  profiles:
    standard:
      mode: acceptEdits      # edits inside the worktree need no approval
      allow:
        - "Bash(git status:*)"
        - "Bash(git diff:*)"
        - "Bash(git log:*)"
        - "Bash(git show:*)"
        - "Bash(git add:*)"
        - "Bash(git commit:*)"
        - "Bash(git switch:*)"
        - "Bash(git rebase:*)"
        - "Bash(git fetch:*)"
        - "Bash(cmake:*)"
        - "Bash(ctest:*)"
        - "Bash(gh pr view:*)"
        - "Bash(gh pr checks:*)"
        - "Bash(gh run view:*)"
        - "Bash(gh issue view:*)"
        - "Skill(contour-workflows:*)"
        - "Read"
        - "Grep"
        - "Glob"
      deny:
        - "Bash(sudo:*)"
        - "Bash(rm -rf:*)"
        - "Bash(git push --force:*)"
        - "Bash(git reset --hard:*)"
    trusted:                 # standard, plus publishing its own branch
      extends: standard
      allow:
        - "Bash(git push:*)"
        - "Bash(gh pr create:*)"
        - "Bash(gh pr edit:*)"
    ask:                     # Claude Code's own default: everything not read-only asks
      mode: default
tasks:
  - { id: fix-ci, …, permissions: trusted }
```

- **Default for every run:** `standard`. The agent edits, builds, tests and commits in its worktree without asking.
  Pushing, opening a PR and anything unlisted still ask, and so reach Needs you.
- **Per task:** `permissions: <profile>` on a task overrides the default (for example, `fix-ci` may push).
- **Per dispatch:** the launcher's settings line gains a `perms` value (`◂▸` cycles the profiles), defaulted from
  the task, so a single run can be given more or less.
- **Off:** `permissions: { default: ask }` restores Claude Code's own prompts.
- **Refused in config:** `bypassPermissions` mode, and a rule naming no tool. A full bypass is never one config line
  away. A profile may only allow and deny named rules.

**Visible and recorded.**
- The run keeps its profile name and the resolved JSON (persisted, additive to `contract/agent-runs/`).
- The Details tab shows `perms standard · 19 allowed · 4 denied`, and expands the rules on `⏎`.
- The launcher's preview shows the profile, and its expanded preview shows the `--settings` value.

**Consent.** The extension now decides what its agents may do unasked, so the manifest's consent text gains: "It
sets which tools the agents it starts may use without asking, from your configuration". The `settings` argument is
in the manifest, so the consent dialog shows it as it shows `board`.

## Attention

- A run that enters **Needs you** moves there. The status line shows `<run> needs you` until you select it. If the
  tab is not active, it requests the attention of the tab, through the existing notification path, if the host
  offers one; no host change otherwise.
- **Unseen markers:**
  - a run whose transcript or board changed since you last looked gets a `•`;
  - the Fleet tab label counts the runs that need you: `Fleet · 2!`.
- Working rows show a core-cpp `Spinner` frame, advanced on the existing redraw tick (no extra timer).

## Mouse (its own late phase)

- **Enabling it.** The guest turns on SGR mouse reporting (`DECSET 1000;1006`) on its own tab through `tab-write`,
  and parses the reports it reads with `tab-read`, through core-cpp's `TerminalInput`. There is no host change.
- **Hit-testing.** Every drawn row, tab, chip and action records its rectangle into a per-frame `HitMap`.
- **What it does:**
  - a click selects a row, switches a tab or runs an action;
  - a double click runs the primary action;
  - the wheel scrolls the pane under the pointer;
  - a click in the message box focuses it.

## Architecture

**Core (small additions):**
- `Run`: `AgentStatus::Asking` (from the split of `Blocked`); the `unseen` digest per run (board hash and
  transcript size as last shown).
- `Engine`:
  - attention ordering for the Fleet;
  - transcript tail (the last three entries) and follow (the last page re-read every five seconds) for the
    selected run, through the existing `agent-transcript` command, at most one in flight;
  - `pressKey(run, PaneKey)` through the host.
- `Config`:
  - `quick_replies`;
  - `permissions` (profiles with `extends`, default, per-task override), resolved by
    `core/Permissions.{hpp,cpp}` into the settings JSON.
- `Run::permissions`: the profile name and resolved JSON.
- `agentPaneArguments()` gains `settings`.

**View (mostly rebuilt; the renderers stay):**

| Unit | What it does |
| --- | --- |
| `view/Fleet.{hpp,cpp}` | groups and orders run rows by attention |
| `view/Focus.{hpp,cpp}` | the header, the three tabs, latest, the chips and the message box; draws with `BoardMarkdown`, `Transcript` and the detail lines from `Sections` |
| `view/Actions.{hpp,cpp}` | `RowState`, `rowStateOf(Row, Engine)`, the `ActionRow` table, `actionsFor()`, `primaryAction()` |
| `view/ActionBar.{hpp,cpp}` | draws the ranked actions that fit |
| `view/Palette.{hpp,cpp}` | adapts the action table and `KeyRow`s to `CommandPalettePopup` |
| `view/Launcher.{hpp,cpp}` | the query, the candidates (items and the topic row), the settings and preview lines; on `FuzzyPickerPopup` |
| `view/Composer.{hpp,cpp}` | per-run drafts, editing, paste, quick replies |
| `view/HitMap.{hpp,cpp}` | mouse phase only |

**Removed:**
- `Screen::Board`, `Screen::Transcript` and their keys;
- the help overlay;
- `DispatchForm`, which is absorbed by `Launcher`, reusing its preview logic;
- `MessageDraft`, absorbed by `Composer`;
- the per-screen `ScreenSet` rows.

**`Interaction`** loses `screen`, `back`, `subject`, `form`, `message` and `overlay`. It gains:
- `Place {Fleet, Composer}`;
- `FocusTab {Board, Transcript, Details}`;
- per-tab scroll and follow;
- `std::optional<LauncherState>` and `std::optional<PaletteState>`.

**Host (quick replies only):** `pane-key` through WIT, `DaemonImport`, `IPaneService`, `DeclaredPanes`,
`OwnedPanes` and `SessionHost`, the same path `pane-send` took; then regenerate the bindings, the manifest contract
and the consent text.

## Visual design

The canvas is at https://claude.ai/artifact/8uWbK1f3w9hhZxdcfBQ8ev, and these are the rules it sets. Every cell is
drawable in a terminal: box drawing, block and braille glyphs, and truecolor with a 256-colour fallback.

**Palette.** One `Theme` table. A light theme is the same table with other values.

| Role | Truecolor [256] |
| --- | --- |
| ground | `#0D1016` [233] |
| panel | `#131821` [234] |
| selection | `#20293A` [236] |
| line | `#2A3446` [237] |
| muted text | `#8D97A8` [246] |
| bright text | `#F3F6FA` [255] |
| accent and focus | `#B4A2FF` [147] |
| asking | `#F4B64E` [215] |
| error | `#FF8A7A` [210] |
| working | `#78B0FF` [111] |
| done | `#66D9A8` [79] |
| faint (rules and borders only) | `#5B6577` [242] |

- Text colours pass 4.5:1 on the panel.
- Every state colour is paired with its own glyph, so nothing is told apart by hue alone.

**State glyphs.** `?` asking, `…` waiting, `✗` error, a braille spinner `⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏` for working, `✓` done, `·`
finished, `⚑` kept. A `•` in the accent colour marks unseen news.

**Top bar.** One line, above the panes:
- the brand mark and host;
- the fleet gauge, `⬢ 3/4` with `▰▱`;
- a `● N need you` pill in the asking colour;
- the clock.

**Two-line agent rows (Wide).**
- **Line 1:** glyph, run, task, then state and age on the right.
- **Line 2:** what the agent is on, in muted text: its pending question, the board's current task (the first open
  `☐`), or its error.
- **Working rows** add a `▰▱` meter, counted from the board's task boxes.
- **Selection:** the row's background, plus a `▍` bar in the accent colour.
- **Medium:** rows fold to one line, with the context after the name.

**Section headers.** Bold, in the group's colour, with a count chip; for example `NEEDS YOU 2`, `WORKING 2 of 4`.

**Focus.**
- **Header:** the run, task and model, with state and age on the right. A second line holds the title, the branch
  and the permission profile.
- **Tab strip:** the active tab is a filled chip with an accent underline beneath it.

**Transcript.**
- Entries carry a coloured left gutter: neutral for claude, the working colour for tools, the asking colour for
  permissions.
- A tool call is one summary line, and `⏎` expands an edit's diff.
- An answered permission stays visible, with who answered and when.
- A `↓ live` chip shows when you have scrolled away from the end.

**Asking box.** An asking-coloured frame above the message box. It names the tool and command, and shows the reply
chips.

**State boxes.** A done run shows a done-coloured box with its actions (open PR, follow up, end) where the message
box would be.

**Details.** Aligned facts, including time and tokens. A PERMISSIONS block shows:
- the profile and where it came from;
- its rules grouped, with what the profile adds over its parent marked;
- how many times the run still asked.

**Launcher, empty query.**
- Aligned CI, review and agent (`⬢`) glyph columns.
- A detail line for the highlighted item.
- The 24-hour timeline leaves the dashboard; the palette's "show the last 24 hours" opens it as a popup.

**Popups (launcher, palette).**
- An accent border on a dimmed dashboard.
- Matched letters underlined in the accent colour.
- In the palette, commands that do not apply are dimmed, with the reason.

**New keys the canvas uses:** `o` (open the item or PR in the browser), `r` (reveal the agent's tab) and `Z` (fold
every group). These go into the `KeyRow` table and the action table.

## Errors

| Situation | What the user sees |
| --- | --- |
| Transcript or board read fails | The tab says why (today's FailureWords); the rest of the Focus still works. |
| Send or key refused | The draft is kept; the action bar shows the reason until the next key. |
| A quick reply on a run no longer asking | Not sent; "pr-2375 is no longer asking". |
| The launcher's query matches nothing | Only the topic row is shown. |
| Narrow tab lower than 20 rows | The tab strip and latest collapse first, then the action bar, as the furniture does today. |

## Testing

- **Actions:** a table test of `rowStateOf` and of `actionsFor` per `RowState`. Every `ActionRow` command must have
  a `KeyRow`, and every state must have exactly one rank-0 action.
- **Fleet:** ordering by attention and age; selection follows the run across a regroup.
- **Focus:** golden screens for each tab, with latest, chips and the message box, at 120×40, 80×24 and 50×20.
  Follow pins and unpins on scroll.
- **Launcher:** filtering, the topic row, the settings cycling, the setup warnings and the second `⏎`, and dispatch
  equivalence with today's form (the same `Dispatch` for the same choice). Goldens at 120×40 and 80×24.
- **Palette:** ordering and dimmed reasons; running a command equals pressing its key.
- **Composer:** per-run drafts, paste, `alt+⏎`, quick replies only while asking with an empty draft.
- **Host:** the `pane-key` policy (owned, declared, enum only), the manifest corpus, the WIT ABI checks, and
  delivery through a fake session.
- **Engine:** the `Asking`/`Waiting` split from hook payloads; tail and follow scheduling (one read in flight, the
  selected run only).
- **Permissions:**
  - profile resolution, including `extends` chains, cycles refused, and unknown names refused;
  - the JSON exactly as Claude Code's `--settings` expects it (a fixture checked against `claude --settings`
    once, by hand);
  - `bypassPermissions` refused;
  - the per-task and per-dispatch overrides;
  - the pane argv for each profile;
  - the persistence round-trip.
- **Mouse:** the hit map per golden layout; SGR report parsing.

## Phases

Each phase builds clean, passes `orchestrator_test` (plus `vtextension_test`, `vthost_test` where touched), and is
usable on its own.

0. **Default permissions.** The `settings` argument, the profiles, the per-task override, Details and consent.
   This phase is independent of the layout, and it cuts the number of `Asking` states first. The launcher's
   `perms` value lands with phase 3.
1. **Fleet + Focus.** The two panes and the left tabs. The Focus tabs replace the board and transcript screens,
   with follow, tail and latest. The message box replaces the message line. Bands and goldens.
2. **Actions and the palette.** `RowState`, the `ActionRow` table, the adaptive action bar, `⏎` as the primary
   action, and the palette replacing the help.
3. **The launcher.** It replaces the dispatch form, `d`, the task letter keys and `n`.
4. **Quick replies.** The live check; the `Asking` split; `pane-key` end to end; `quick_replies`; the chips.
5. **Attention.** Needs-you notices, unseen markers, spinners.
6. **Mouse.**

## Out of scope

- Reading a pane's screen. The transcript remains the way to see what an agent said.
- Several machines in one dashboard (unchanged from the previous design).
- Persisting drafts across daemon restarts.
- Configurable key bindings.
