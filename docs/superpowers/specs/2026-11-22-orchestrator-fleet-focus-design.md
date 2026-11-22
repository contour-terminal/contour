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
nothing to memorise beyond `↑↓ ⏎ esc n ^K`, plus `1 2 3` once you have seen an agent ask. What the screen offers
adapts to what is selected, and the action bar always names what `⏎` will do.

Three rules hold everywhere, and the rest of this spec follows from them:

- **A letter never does two things.** In the Fleet a letter is a command; in a text field (the message box, the
  launcher, the palette, the filter) every printable key types. There is no "start typing to write" shortcut.
- **`⏎` acts, `esc` steps back.** `⏎` does what the action bar lists first. `esc` closes the innermost popup or
  field and loses nothing: a draft, a query and a selection survive it. `esc` never sends, ends or answers anything.
- **Every action says what happened.** Its result shows on the action bar until the next key (see "Feedback").

## Layout

```
╭─ Agents 7 ─────────────────────────╮ ╭─ pr-2375 · fix CI · sonnet ──────────── ● asking 2m ──╮
│ NEEDS YOU · 2                      │ │  Board   Transcript   Details                          │
│ ▶ pr-2375  fix CI          asking 2m│ │ ━━━━━━                                                 │
│   topic    flaky reflow   error    │ │ ## Fixing CI                                           │
│ WORKING · 2 OF 4                   │ │ ☑ reproduced on master                                 │
│   issue-2338  fix          ⠋ 12m   │ │ ☐ fix the reflow   ← now                               │
│   topic  reflow docs       ⠋  3m   │ │                                                        │
│ TO REVIEW · 1                      │ │ ── latest ──────────────────────────────────────────── │
│   pr-2369  fix CI     PR updated   │ │ 14:30 claude The pending wrap is now cleared…          │
│ FINISHED · 3                       │ │ ╭ ? pr-2375 asks ────────────────────────────────────╮ │
│   pr-2366  completed      removed  │ │ │ Bash  ctest -R reflow --output-on-failure          │ │
│                                    │ │ │ 1 yes   2 always   3 no, and say why               │ │
│ + new agent                        │ │ ╰────────────────────────────────────────────────────╯ │
╰────────────────────────────────────╯ ╰────────────────────────────────────────────────────────╯
 1 2 3 answer · ⏎ see request · e end · n new · ^k all actions                ⬢ 3/4 · 2 need you
```

While the selected agent asks, the asking box takes the message box's place: Claude Code's menu is not reading
messages, so there is nothing to type into (see "Quick replies"). The sketch shows the Board because the user
chose it with `b`; an asking run otherwise opens on its Transcript, at the request.

### Left pane: the Fleet, agents only

- The Fleet lists runs only, grouped by **what they need from you**, in this order:
  - **Needs you:** asking (a permission prompt), waiting for input (including an agent whose turn ended without
    reporting done, hook status `idle`), or stopped by an error;
  - **Working:** working, or starting (its pane is open, no hook has reported yet);
  - **To review:** the agent reported done, and the run is still open;
  - **Finished:** every recorded run not dismissed, newest first, folded to its newest five. `⏎` on the header
    unfolds it, and `/` searches all of it, so yesterday's run is a filter away.

  Within a group the longest-waiting run comes first, so `home` always selects the run that has waited longest
  for you. A run moves between groups as its state changes, and the selection stays on the run, not on the
  position; a run entering Needs you never takes the selection.
- Pull requests, issues and Jira issues are no longer listed on the dashboard. You browse and start them from the
  launcher (`n`), which opens on the items that need attention.
- `+ new agent` is the last Fleet row; `⏎` on it opens the launcher.

**Empty and first run.** With no runs, the Fleet shows only `+ new agent`, selected, and the Focus shows a
three-line primer in place of a run:

```
No agents yet. ⏎ or n starts one on a pull request, an issue, or anything you type.
Once agents run: ⏎ talks to the selected one, 1 2 3 answers one that asks, esc steps back.
Everything else is in ^K.
```

The primer returns whenever the Fleet is empty. Unconfigured, the Fleet shows today's "Nothing to list until the
orchestrator is configured" text, and `n` says what is missing instead of opening the launcher.

### Right pane: Focus, the selected run

The header shows the run, its task, its model and its state with its age.

There are three tabs, each on its own letter, which the tab strip underlines: **`b` Board**, **`t` Transcript**,
**`d` Details**. They work from the Fleet and from the Focus; in a text field they type, as every letter does.

- **Board:** the rendered `STATUS.md`, re-read every five seconds, as the details panel shows it today.
- **Transcript:** follows the agent while it runs. It shows the last page, re-read every five seconds, and stays
  pinned to the end unless you scroll up. A `↓ live` chip shows while you are away from the end; `end` returns.
- **Details:** what the details panel lists today: branch, worktree, URL, checks, kept reasons, failure words and
  the permissions the run was started with.

**Which tab opens.** A run opens on the tab its state names in the action table ("Opens on"). A tab you choose
with `b`, `t` or `d` sticks for that run until its state changes, so a run that starts asking shows you the
request even if you left it on Board.

Under the tab, **latest** shows the last three transcript entries, unless the Transcript tab is open.

Nothing inside a tab takes `⏎`: there is no cursor in the Focus. An edit's diff shows its first eight lines and
`+N lines`; the Transcript tab, or the agent's own tab (`r`), shows the rest.

The bottom of the Focus depends on the run's state:

| State | Bottom of the Focus |
| --- | --- |
| Asking | the **asking box**: tool, command, and the reply chips `1 2 3` |
| Starting, Working, Waiting, Errored | the **message box**: `⏎ to write to pr-2375` when the Fleet has the keys, a cursor when it has them |
| Done | a done box: `✓ reported done`, and `⏎ follow up · o open PR · e end` |
| Finished, Kept | how it ended, and its actions |

### Bands

The user is in one of two **places**, the **Fleet** or the **Focus**, shown by the accent border. The band decides
only what stays visible while you are in the Focus:

| Band | Fleet place | Focus place |
| --- | --- | --- |
| **Wide** (≥ 100 columns) | both panes, side by side | both panes, side by side |
| **Medium** (60–99) | Fleet above, Focus below at 60 % of the height | the Focus takes the whole body |
| **Narrow** (< 60) | the Fleet alone | the Focus alone |

There is no other depth. In every band, `⏎` on a live run (and `tab` on any run) goes to the Focus, and `esc` comes
back. The top bar's `● N need you` pill stays visible in every band, so a new request shows while you type.

**Small terminals.** At 80×24 (Medium) and 50×20 (Narrow) the furniture gives way in this order, so the content
keeps at least eight rows:

1. the Focus header's second line (title, branch, profile);
2. latest, while the Board tab is open;
3. the top bar, whose gauge and pill move to the right end of the action bar;
4. the tab strip, whose active tab moves into the header as `· board`;
5. the Fleet's second row lines, except for Needs you rows, whose question or error stays, so `1 2 3` can be
   answered from the Fleet without opening anything.

The message box is one line while empty and grows with its draft, up to a third of the Focus.

## Places and keys

**Fleet (the default).** Letters are commands:

| Key | Does |
| --- | --- |
| `↑↓` `jk` `home` `end` | move; `home` is the run that has waited longest for you |
| `⏎` | the selected row's **primary action**, which the action bar lists first |
| `tab` | to the Focus |
| `esc` | clear the filter; otherwise nothing |
| `pgup` `pgdn` | scroll the Focus tab |
| `b` `t` `d` | the Focus tab |
| `n` | the launcher |
| `/` | filter the rows |
| `^K` `?` | the command palette |
| `1` `2` `3` | answer the selected agent, while it asks and its asking box (or, Narrow, its question) is on screen |
| `e` `o` `r` `x` `del` `z` | end, open PR, reveal the agent's tab, retry removal, dismiss, fold; offered only where they apply |

`m`, `R`, `I`, `Z`, `[` and `]` are gone as keys: `⏎` writes to any live run, and refresh, install plugins and
fold all are palette commands.

**Focus.** For a run with a message box, the box has the keys:

| Key | Does |
| --- | --- |
| printable keys, digits included | type |
| `⏎` | send |
| `alt+⏎` | new line; a paste keeps its line feeds |
| `^←` `^→` | move by word, as in any text field |
| `pgup` `pgdn` `end` | scroll the tab; `end` back to live |
| `esc` | back to the Fleet, keeping the draft (drafts are kept per run, in memory) |
| `^K` | the command palette |

For any other run (asking, done, finished), the Focus takes the Fleet's letters and digits, and `↑↓` scroll
instead of moving.

**After a send** the draft clears, the message shows in latest as `you · sent 14:31`, the keys return to the Fleet,
and the selection stays on the run. A refused send keeps the draft, keeps the keys in the box, and shows why on
the action bar, as the message line does today.

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

`RowState` is one enum. The ranked actions for each state, and the Focus tab it opens on:

| RowState | Group | Actions, rank order | Opens on |
| --- | --- | --- | --- |
| `Asking` | Needs you | `1 2 3 answer` · `⏎ see request` · `r agent tab` · `e end` | Transcript, at the request |
| `Waiting` | Needs you | `⏎ reply` · `t transcript` · `b board` · `e end` | Transcript |
| `Errored` | Needs you | `⏎ reply` · `t transcript` · `d details` · `e end` | Transcript |
| `Starting` | Working | `⏎ message` · `r agent tab` · `e end` | Board |
| `Working` | Working | `⏎ message` · `t transcript` · `b board` · `e end` | Board |
| `Done` | To review | `⏎ follow up` · `o open PR` · `t transcript` · `e end` | Board |
| `Finished` | Finished | `⏎ run again` · `t transcript` · `del dismiss` | Details |
| `Kept` | Finished | `⏎ retry removal` · `d details` · `del dismiss` | Details, at the kept reasons |
| `NewAgent` | — | `⏎ new agent` | the primer, while the Fleet is empty |
| `GroupHeader` | — | `⏎ fold` | — |

- **`⏎` on a live run always writes to it.** The label changes (reply, message, follow up); the action does not.
  The one exception is `Asking`, where the menu takes keys, not messages, so `⏎` shows the request instead.
- **The bar shows the actions that fit, in rank order, then `^k all actions`.** For `Asking` the chips come first,
  because the digits are the answer.
- An action that does not apply to a state has no row, so it is never offered.
- Adding a state or an action is one row.
- The existing `KeyRow` table stays the source of key → command. `ActionRow` only says which commands apply, in
  what order, and which tab a state opens on.
- A run whose agent fails before its pane opens is `Finished`, with the failure words on its second line and in
  Details; `⏎ run again` opens the launcher with the same choice.

## Feedback, confirmation and undo

Every action answers on the action bar, in the colour of its outcome, until the next key.

| Action | Asks first? | What you see after it |
| --- | --- | --- |
| Start (launcher `⏎`) | only with a setup warning: `⏎ again to start anyway` | the launcher closes; the new run is selected in Working as `starting`, its Board says `waiting for the agent's first report` |
| Send a message | no | `sent to pr-2375`; the message in latest as `you · sent 14:31` |
| Quick reply `1 2 3` | no | the chip fills, `answered yes`; the asking box stays, dimmed, until the hook leaves asking. After 10 s still asking: `pr-2375 still asks · r opens its tab` |
| `3` no, and say why | no | as above, then the message box opens for that run, empty, so the next keys are the reason |
| End `e` | yes, once: `end pr-2375? e again to end · esc keeps it` (as today) | the run moves to Finished, `removed`, or `kept` with the reason |
| Dismiss `del` (Finished only) | no: it changes no files | the row leaves the Finished group; `dismissed pr-2366 · ^K show dismissed brings it back` |
| Retry removal `x` | no: removal is never forced | `removed`, or the reason it is still kept |
| Open PR `o` | no | `opening #2369 on your machine`; before phase 6, `copied #2369's link · ctrl+click it to open` |

## The command palette (`^K`, `?`)

core-cpp's `CommandPalettePopup` lists every command:
- the selected row's actions first, from the table above;
- then the global ones: new agent, filter, refresh the lists, install the missing plugins, fold every group, show
  the last 24 hours (the timeline, as a popup);
- each with its key, or `palette only`.

Typing filters it fuzzily, `⏎` runs the chosen command, and `esc` closes it. It replaces the help overlay: `?`
opens it too. Commands that do not apply to the selection are listed dimmed, with the reason (for example, "e end
· no run selected"). The palette holds the rarely-used commands, so the Fleet needs no key for them.

## The launcher (`n` or `+ new agent`): browse and start

```
╭─ New agent ────────────────────────────────────────────╮
│ › flaky reflow█                                         │
│   ✎ topic "flaky reflow"                      ⏎ start   │
│   #2338 Flaky on Windows: SessionHost…   issue contour  │
│   #2375 Say why the stdio relay failed…  PR    contour  │
│─────────────────────────────────────────────────────────│
│ task fix · model sonnet · perms standard · repo contour │
│ new branch agent/topic-flaky-reflow-… in ~/worktrees/…  │
╰─────────────────────────────────────────────────────────╯
```

It is one popup, built on `FuzzyPickerPopup` for the list. It replaces the item lists on today's dashboard,
`DispatchForm`, `d`, the task letter keys and `n`. The query field has the keys, so every printable key types; the
launcher has no letter commands.

| Key | Does |
| --- | --- |
| printable keys | type the query |
| `↑↓` | move in the list |
| `tab` `shift+tab` | move through the settings (task, model, perms, repo) and back to the list |
| `←→` | change the focused setting; in the query, move the cursor |
| `⏎` | start the highlighted row, from anywhere in the popup |
| `alt+p` | expand the full prompt and command line, as the dispatch form shows them today |
| `esc` | close, starting nothing |

- **Empty query: browse.** The items are listed in three groups:
  - **Needs attention:** your PRs with failing CI or requested changes;
  - **My pull requests;**
  - **Issues without a PR** and Jira.

  Each group is ordered as today's lists are. CI, review and "an agent is on it" (`⬢`) are aligned glyph columns.
  One detail line describes the highlighted item: the failing job, the checks count, the live agent's state, and
  its URL as an OSC 8 link the user's own terminal can open, wherever the daemon runs.
- **The query** filters the PRs, issues and Jira issues of every configured repository, matching their number and
  title. While the query is not empty, it always offers **topic "<query>"**:
  - the topic row is first and highlighted;
  - except when the query names an item (`2375`, `#2375`, `CT-412`, or a pasted PR or issue URL) and that item
    exists: then the item is first and highlighted, and the topic row second. Typing a number never starts a topic
    called "2375".
- **Starting on an item that already has a live agent** highlights `⬢` and asks once: `pr-2375 already runs ·
  ⏎ again starts a second agent · esc`.
- **The settings line** shows the task, model, permission profile and repository for the highlighted row:
  - the task is the first that applies to it;
  - the model and the profile are the task's;
  - the repository is the item's, or for a topic the one last used for a topic (the first configured, at first).
- **The preview line** shows the checkout (new branch or PR head), the worktree and the profile.
- **Setup warnings** keep the current rule: missing hooks, missing plugins, or the run limit reached. The warning
  is drawn on the settings line with what fixes it (`plugins missing · ^K install`), and `⏎` asks again before it
  dispatches.
- **The lists are read when the launcher opens** if they are older than a minute; until then the groups show a
  spinner, and the topic row already works. If they cannot be read, the browse area says why, and topics still
  start.
- Opened from an item, the query is the item's number and the item is highlighted.
- From a finished run, `run again` opens it with that run's item or topic, task, model and profile.

## Quick replies

While the selected agent is **asking**, Claude Code shows its select menu (1 yes · 2 yes, and don't ask again ·
3 no). The Focus shows the asking box with the replies as chips, and `1`, `2` or `3` answers.

- **Where the digits work.** In the Fleet, and in the Focus of an asking run. There is no message box while a run
  asks, so a digit never has to choose between answering and typing.
- **Only what you can see.** A digit answers only while the selected run's request is on screen: the asking box,
  or in Narrow and at 80×24 the row's second line. Otherwise it does nothing and the bar says `select the
  request first`.
- **`3` leaves the agent waiting for a reason.** The run turns `Waiting`, and the message box opens for it.

**Asking versus waiting.** `AgentStatus::Blocked` splits into `Asking` and `Waiting`, from the status hook's
Notification `notification_type` (`permission_prompt` versus `idle_prompt`). Where the hook cannot tell, the run is
`Waiting` and shows no chips: you answer it in its tab (`r`), and its second line says so.

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
  from a config table, `quick_replies:`, so a changed Claude Code menu is fixed in config. The defaults follow
  Claude Code's own wording, shortened: `1 yes`, `2 yes, don't ask again`, `3 no, and say why`.
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

**Profiles** are a data table. The built-in ones are written out as YAML below for reading. Configuration picks
the default with `permissions: <name>`, and adds profiles or replaces a built-in one through
`permission_profiles`, a list of `{ name, extends, mode, allow, deny }`. It is a list, not a map, because the
configuration's schema walk reads lists of objects:

```yaml
# built in: the profiles every configuration has
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
- **Fixed for the run's life.** Claude Code reads `--settings` once, at launch, so a running agent's profile cannot
  change. To widen one run, answer `2 always` when it asks; to change the profile, end it and `run again`, which
  opens the launcher with the run's choice and lets you pick another `perms`. Details says this beside the profile:
  `standard · fixed at start · run again to change`.
- **Off:** `permissions: ask` restores Claude Code's own prompts.
- **Refused in config:** `bypassPermissions` mode, and a rule naming no tool. A full bypass is never one config line
  away. A profile may only allow and deny named rules.

**Visible and recorded.**
- The run keeps its profile name and the resolved JSON (persisted, additive to `contract/agent-runs/`).
- The Details tab shows `perms standard · 19 allowed · 4 denied`, and lists the rules below it, grouped.
- The launcher's preview shows the profile, and its expanded preview shows the `--settings` value.

**Consent.** The extension now decides what its agents may do unasked, so the manifest's consent text gains: "It
sets which tools the agents it starts may use without asking, from your configuration". The `settings` argument is
in the manifest, so the consent dialog shows it as it shows `board`.

## Attention

- A run that enters **Needs you** moves there, without taking the selection. The top bar's `● N need you` pill
  counts it, and the action bar's right end names it (`pr-2375 needs you · home`) until you select it. If the tab
  is not active, it requests the attention of the tab, through the existing notification path, if the host offers
  one; no host change otherwise.
- **Unseen markers:**
  - a run whose transcript or board changed since you last looked gets a `•`, and so does the tab that changed;
  - the dashboard's own terminal tab title counts the runs that need you, `Orchestrator · 2!`, written as OSC 2
    through `tab-write`, so a user in another tab sees it.
- Working rows show a core-cpp `Spinner` frame, advanced on the existing redraw tick (no extra timer).

## Links: the terminal you are at opens them

A URL is opened by the machine you are sitting at, not by the daemon's machine. When you attach to a remote
daemon, those are different, and only your terminal knows where you are.

- **Every link is a hyperlink.** The orchestrator draws every URL it shows as an OSC 8 hyperlink: the PR and
  issue links, the URLs in boards and transcripts, and the checks. It uses core-cpp's `HyperlinkEmitter`, which
  frames a buffer's hyperlink regions while its cells flush. Contour, and any terminal with OSC 8, underlines
  them on hover and opens them with ctrl+click (Contour's `FollowHyperlink`, through its external launcher, on
  the client's machine). This needs no host change, and it is in phase 1.
- **`o` opens without the mouse.** A new daemon-world import asks the frontend whose key press it serves to
  follow a link:

  ```wit
  /// Asks the frontend the user pressed a key in to open an https link, as FollowHyperlink does.
  url-open: func(url: string, origin: origin) -> result<_, host-error>;
  ```

  - **Policy:** the host accepts `https://` URLs only, at most 2 KiB, and only with an origin from a key the user
    pressed in the extension's own tab within the last 2 s (the rule `tab-reveal` already uses). The manifest
    declares `url-open`, and the consent dialog says "It may ask your terminal to open https links you choose".
  - **Routing:** the host sends the request to that frontend, over the native protocol as a new PDU. The frontend
    opens it with its existing external launcher, the same path a ctrl+click takes. A frontend without a
    launcher answers `not-supported`.
  - **Before this lands,** or when it is refused, `o` copies the link with OSC 52 and says
    `copied #2369's link · ctrl+click it to open`.

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
- `Run`: `AgentStatus::Asking` (from the split of `Blocked`); `Starting` until the first hook reports; the
  `unseen` digest per run (board hash and transcript size as last shown).
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
| `view/Composer.{hpp,cpp}` | per-run drafts, editing, paste |
| `view/HitMap.{hpp,cpp}` | mouse phase only |

**Removed:**
- `Screen::Board`, `Screen::Transcript` and their keys;
- the help overlay;
- `DispatchForm`, which is absorbed by `Launcher`, reusing its preview logic;
- `MessageDraft`, absorbed by `Composer`;
- the per-screen `ScreenSet` rows;
- the keys `m`, `R`, `I`, `[`, `]` and the task letter keys; refresh and install plugins move to the palette.

**`Interaction`** loses `screen`, `back`, `subject`, `form`, `message` and `overlay`. It gains:
- `Place {Fleet, Focus}` (the Focus of a run with a message box gives the box the keys);
- `FocusTab {Board, Transcript, Details}`, with the per-run choice that sticks until the state changes;
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
- the brand mark and host (the host matters when you supervise a remote machine);
- the fleet gauge, `⬢ 3/4` with `▰▱`;
- a `● N need you` pill in the asking colour;
- a setup warning, only while something is missing (`plugins missing · ^K install`);
- the clock.

**Bottom line.** One line: the ranked actions on the left, and on the right the newest notice
(`pr-2375 needs you · home`). There is no separate status line; setup checks that pass are not shown. Feedback
for an action replaces the actions until the next key.

**Two-line agent rows (Wide).**
- **Line 1:** glyph, run, task, then state and age on the right.
- **Line 2:** what the agent is on, in muted text: its pending question, the board's current task (the first open
  `☐`), or its error.
- **Working rows** add a `▰▱` meter, counted from the board's task boxes.
- **Selection:** the row's background, plus a `▍` bar in the accent colour.
- **Medium:** rows fold to one line, with the context after the name.

**Section headers.** Bold, in the group's colour, with a count chip: `NEEDS YOU 2`, `WORKING 2 of 4`,
`TO REVIEW 1`, `FINISHED 3`. These four names are the only group names, in the spec and on screen.

**Focus.**
- **Header:** the run, task and model, with state and age on the right. A second line holds the title, the branch
  and the permission profile.
- **Tab strip:** the active tab is a filled chip with an accent underline beneath it; each label's key letter
  (`B`oard, `T`ranscript, `D`etails) is underlined. No `[ ] switch` hint.

**Transcript.**
- Entries carry a coloured left gutter: neutral for claude, the working colour for tools, the asking colour for
  permissions.
- A tool call is one summary line; an edit shows its first eight diff lines, then `+N lines`.
- An answered permission stays visible, with who answered and when.
- A `↓ live` chip shows when you have scrolled away from the end.

**Asking box.** An asking-coloured frame in the message box's place (there is no message box while a run asks).
It names the tool and command, and shows the reply chips with their digits: `1 yes  2 always  3 no, and say why`.
The chip labels come from `quick_replies:`, never from the pane.

**State boxes.** A done run shows a done-coloured box with its actions (follow up, open PR, end) where the message
box would be. The message box, unfocused, reads `⏎ to write to pr-2375`; focused, it has a cursor and the bar
reads `⏎ send · alt⏎ line · esc keep draft`.

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

**New keys the canvas uses:** `o` (open the run's PR) and `r` (reveal the agent's tab). They go into the `KeyRow`
table and the action table. Fold every group is a palette command, not a key; the launcher has no `o` (its detail
line carries the link), since its query takes every letter.

## Errors

| Situation | What the user sees |
| --- | --- |
| Transcript or board read fails | The tab says why (today's FailureWords); the rest of the Focus still works. |
| Send or key refused | The draft is kept; the action bar shows the reason until the next key. |
| A quick reply on a run no longer asking | Not sent; "pr-2375 is no longer asking". |
| The launcher's query matches nothing | Only the topic row is shown. |
| The launcher's lists cannot be read | The browse area says why; topics still start. |
| A quick reply that does not take effect | After 10 s still asking: "pr-2375 still asks · r opens its tab". |
| A run's agent fails before its pane opens | The run is `Finished` with the failure words; `⏎ run again` reopens the launcher with the same choice. |
| `o` refused or unsupported by the frontend | The link is copied with OSC 52 through `tab-write`: "copied #2369's link · ctrl+click it to open". |
| A tab lower than its furniture needs | The furniture gives way in the order under "Small terminals". |

## Testing

- **Actions:** a table test of `rowStateOf` and of `actionsFor` per `RowState`. Every `ActionRow` command must have
  a `KeyRow`, and every state must have exactly one rank-0 action.
- **Fleet:** ordering by attention and age; `idle` and `idle_prompt` land in Needs you, a run with no report yet
  in Working as `starting`; selection follows the run across a regroup and is never taken by a new Needs you run;
  the empty-Fleet primer.
- **Focus:** golden screens for each tab, with latest, the asking box and the message box, at 120×40, 80×24 and
  50×20, including the furniture order. Follow pins and unpins on scroll. The opening tab per state, and a chosen
  tab sticking until the state changes.
- **Launcher:** filtering, the topic row (and an item reference outranking it), the live-agent second `⏎`, the
  settings cycling, the setup warnings and the second `⏎`, and dispatch
  equivalence with today's form (the same `Dispatch` for the same choice). Goldens at 120×40 and 80×24.
- **Palette:** ordering and dimmed reasons; running a command equals pressing its key.
- **Keys:** no letter is both a command and text in one place; digits type in the message box and answer only
  while a request is on screen; `esc` never sends, ends or answers.
- **Composer:** per-run drafts, paste, `alt+⏎`; after a send the keys return to the Fleet.
- **Feedback:** each row of the feedback table shows its notice; `e` asks once.
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
1. **Fleet + Focus.** The two panes, the empty-Fleet primer and the small-terminal order. The Focus tabs replace the board and transcript screens,
   with follow, tail and latest. The message box replaces the message line. Bands and goldens.
2. **Actions and the palette.** `RowState`, the `ActionRow` table, the adaptive action bar, `⏎` as the primary
   action, and the palette replacing the help.
3. **The launcher.** It replaces the dispatch form, `d`, the task letter keys and `n`.
4. **Quick replies.** The live check; the `Asking` split; `pane-key` end to end; `quick_replies`; the chips.
5. **Attention.** Needs-you notices, unseen markers, spinners.
6. **Mouse and `url-open`.** Click, wheel and the hit map; `url-open` through WIT, vtextension, the native
   protocol and the frontend's launcher.

## Decisions taken

- **Tabs:** `b` `t` `d`, not `[` `]`: a letter names what it shows.
- **Kept runs:** stay a row state (`⚑`, `⏎` retries the removal) inside the Finished group, not a group of their
  own.
- **Dismiss:** does not ask, and has no undo key. It only hides the record, and the palette's "show dismissed runs"
  lists hidden runs, where `⏎` brings one back.
- **Quick-reply labels:** come from config, never from the screen. The defaults follow Claude Code's wording.
- **Links:** OSC 8 everywhere from phase 1; `o` through `url-open` in phase 6, with an OSC 52 copy until then.

## Out of scope

- Reading a pane's screen. The transcript remains the way to see what an agent said.
- Several machines in one dashboard (unchanged from the previous design).
- Persisting drafts across daemon restarts.
- Configurable key bindings.
