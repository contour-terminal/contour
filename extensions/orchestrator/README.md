# Agent orchestrator

## User story

> As a developer who hands the routine part of my pull-request and issue work to Claude Code agents,
> I want one place that shows what needs doing across my repositories and what my agents are doing
> about it, so that starting an agent is "pick an item, press Enter" and cleaning up after it is
> nobody's job — and so that I can run several agents in parallel without the bookkeeping becoming
> the bottleneck.

Today every agent run is a manual ritual: notice that CI failed on a PR, that a reviewer left
comments, or that an issue has no PR; create a git worktree and a branch; open a tab, `cd`, start
`claude`, type `/fix-ci <url>`; remember which tab is which; later work out which agents are working,
which wait for an answer and which are done; and finally remove worktrees without deleting one that
still holds work. The orchestrator does that ritual. The user keeps the decisions: every dispatch is
confirmed, nothing starts on its own.

When an agent has been at it for a while, I want to know where it stands without switching to its tab and
scrolling: what it says it is doing, and what it actually did. An agent asked to keep a `STATUS.md` has a
board I read from the dashboard, with its age; every run has a transcript I page through, also after it
finished and its worktree is gone; and from either I start the next run on the same item.

- **Success:** four agents dispatched in a minute; their state visible at a glance from any window
  attached to the daemon, or from none; worktrees gone when their runs end, unless they hold work; what a
  finished run did readable without its worktree.
- **Non-goals:** automatic dispatch; deciding what an agent may do. Claude Code's permission mode,
  the agent's skills and memory, and the forge's branch protection decide that — including whether an
  agent may push to a protected branch.
- **Anti-goals (a failure even if it works):** an agent's work lost by cleanup; the extension typing
  anything into a pane that you did not write and send, reading a screen, or choosing Claude's settings;
  untrusted PR or issue text reaching a prompt, a branch name or a path.

## On Windows

The host hands the orchestrator `worktree_root` and each `repos[].path` in their resolved backslash spelling
(`C:\…` or `\\server\share\…`), whichever way the configuration wrote them (`C:\…`, `C:/…`, `\\server\share\…`,
`~\…`). The orchestrator works out the host's style from that spelling, refuses a repository spelled in the other
style or that Win32 cannot address, and compares roots ignoring ASCII case. It builds worktree paths with `\`
(`C:\…\wt\<repo>\<run>`) and reads git's forward-slash output against them; the paths it passes to `file-read`
stay `/`-separated and relative to the worktree root. A command that fails with a negative exit code — an
NTSTATUS — is shown in hex (`it exited with 0xC0000005`).

## The dashboard

The orchestrator draws its own tab (`view/`, drawn with core-cpp's headless terminal UI). It is one screen to
see, read and talk to agents: the **Fleet** on the left lists your runs, and the **Focus** on the right shows
the one selected. Pull requests and issues are not listed here: you browse and start them from the launcher
(`n`, below).

A top bar names the dashboard and carries the fleet gauge (`⬢ 3/4 ▰▰▰▱`, active runs of `max_agents`), a
`● 2 need you` pill while a run waits for you, and `plugins missing · ^K install` while a plugin the tasks
need is missing. Problems — a failing `gh`, a list cut at its limit, a configuration that does not parse — sit
below it, one line each. The bottom line is the action bar: what you can do with the selected row, or the
result of the last action until the next key (see "Actions and the palette" below).

**The Fleet** groups the runs by what they need from you:

| Group | Holds |
| --- | --- |
| NEEDS YOU | an agent that asks for a permission (`?`, at Claude Code's menu, which `1 2 3` answer), waits for you (`…`, its turn ended without done, or it asked a question), or stopped by an error (`✗`) |
| WORKING | an agent at work (a turning `⠋` spinner), starting (`◌`, its tab open and no report yet), preparing or ending |
| TO REVIEW | an agent that reported done (`✓`), its run still open |
| FINISHED | every run kept and not dismissed: removed (`·`), kept (`⚑`) or failed (`✗`), folded to the newest five |

Within a group the run that has waited longest comes first, so `Home` selects the one that has waited longest
for you; Finished lists the newest first. A run moves between groups as its state changes, and the selection
stays on the run. In a tab 100 columns or wider a run's row takes two lines: its glyph, name, task, state and
age, and below them, dimmed, what it is on — the reason its agent reported, its board's first open `☐`, what
it needs from you, or how it ended — with a `▰▱` meter of its board's task boxes once its board was read. In a
narrower tab a row is one line, but a Needs you row keeps its second line, so that its question shows without
opening anything. While a worktree is left that no run accounts for, a LEFTOVER WORKTREES group lists it (see
below). `+ new agent` is the last row; with no run it is the only one, and the Focus shows a primer. Every
title is sanitised before it is drawn, so an escape sequence in a title is shown as text and does nothing.

**The Focus** shows the selected run in a box whose border names it (`pr-2375 · fix CI · sonnet`) and its
state. Below are its item's title, its branch and its permission profile, then three tabs:

- **Board** (`b`): its `STATUS.md`, rendered, read every five seconds while it runs.
- **Transcript** (`t`): its Claude Code session log. While the agent runs, the last page is read again every
  five seconds and the tab stays pinned to its end; scrolled up, it stops following and a `↓ live` chip shows,
  and `End` returns to it. `←` `→` turn the pages.
- **Details** (`d`): its task, model, state, statistics, branch, worktree, URL and permissions, and for a
  finished run its failure in plain words and what kept its worktree.

A run opens on the tab its state names — one that needs you on its Transcript, one at work or done on its
Board, a finished one on its Details — and a tab you choose sticks for that run until its state changes, so a
run that starts asking shows you its request. Under the tab, `latest` shows the last three transcript entries
(and a message you sent, until the transcript does), except while the Transcript tab is open. At the bottom
is the message box of a live run, the asking box of one that asks (below), a done run's `✓ reported done`
box, or how a finished run ended.

**Links.** `o` opens the selected run's pull request or issue on the machine you sit at, not the daemon's:
the orchestrator asks the host's `url-open`, which hands the link to the window you pressed the key in, and
that window opens it with its own launcher, as a hyperlink you follow there would. The action bar says
`opening #2369 on your machine`, or why not (`not opened: this terminal cannot open links`). Every URL the
dashboard draws is also recorded as an OSC 8 hyperlink, and a click on one opens it the same way. (A served
tab does not pass OSC 8 or OSC 52 on to the terminal, so ctrl+click and copying do not apply there.)

**The mouse.** The dashboard asks for the mouse (SGR reporting, DECSET 1000 and 1006). A click selects a row of
the Fleet, and a double click runs its primary action; one click on `+ new agent` opens the launcher. A click
switches the Focus's tab, answers with a quick-reply chip, gives the message box the keys, opens a link, runs a
palette entry, or highlights a launcher candidate (a double click starts it). A click beside an open popup
closes it. The wheel moves the Fleet's selection, scrolls the Focus's tab, or moves a popup's highlight,
whichever is under the pointer. Each frame records what it drew where, so a click lands on what you see.
Shift+drag still selects text in Contour.

**Places and keys.** The Fleet has the keys by default, and its letters are commands; the Focus gets them with
`Enter` on a live run or `Tab` on any, and its border lights up. In the Focus of a live run the message box
has the keys: every printable key types, digits too, so no letter is ever both a command and text.

| Key | In the Fleet |
| --- | --- |
| `↑` `↓` `j` `k` `Home` `End` | move the selection |
| `Enter` | the selected row's primary action, which the action bar lists first (below) |
| `Tab` | give the Focus the keys |
| `b` `t` `d` | show the Focus's Board, Transcript or Details tab |
| `PgUp` `PgDn` | scroll the Focus's tab |
| `←` `→` | turn the transcript's pages; on the Board or Details, or past the transcript's first or last page, show the tab beside |
| `n` | open the launcher: browse the pull requests and issues, or type a topic |
| `/` | filter the runs by text, Finished's folded ones too; Enter keeps the filter, Esc drops it |
| `Ctrl+K` `?` | open the command palette: every action, and the commands that have no key |
| `e` | end the selected run; asks once (`e again to end · esc keeps it`) |
| `r` | show the selected agent's tab |
| `o` | copy the run's pull request or issue link |
| `x` | check a kept worktree again |
| `Del` | drop a finished run from the list |
| `z` | fold the selected run's group, or unfold a folded one |
| `1` `2` `3` | answer the selected agent that asks, with that quick reply (below) |
| `Esc` | drop the filter |

A run key acts only where it applies to the selected row; elsewhere the action bar says why (`del dismiss ·
not while it is working`) and nothing happens.

| Key | In the Focus, with the message box |
| --- | --- |
| printable keys | type |
| `Backspace` `Alt+Backspace` `Ctrl+W` `Ctrl+U` | drop a character, a word, back to the last blank, everything |
| `Enter` | send; the keys return to the Fleet, the selection stays on the run |
| `Alt+Enter` | add a line; a paste keeps its line feeds |
| `PgUp` `PgDn` `End` | scroll the tab; `End` back to live |
| `←` `→` | as in the Fleet: pages, then the tab beside |
| `Esc` `Tab` | back to the Fleet, keeping the draft (drafts are kept per run, in memory) |
| `Ctrl+K` | open the command palette |

In the Focus of a run without a message box — one finished, preparing or ending, or a leftover worktree — the
Fleet's letters act, and `↑` `↓` scroll instead of moving. `Esc` or `Tab` goes back.

**Actions and the palette.** What the dashboard offers follows the selected row. Each row's state has its
actions in rank order; the first is what `Enter` does, and the action bar lists them while they fit, then
`n new agent` and `^k all actions`:

| Selected | Action bar |
| --- | --- |
| a run that asks | `1 2 3 answer` · `⏎ see request` · `r agent tab` · `e end` |
| a run that waits, or stopped by an error | `⏎ reply` · `t transcript` · `b board` (or `d details`) · `e end` |
| a run starting or at work | `⏎ message` · `t transcript` · `b board` · `e end` (starting: `r agent tab`) |
| a run that reported done | `⏎ follow up` · `o open link` · `t transcript` · `e end` |
| a finished run | `⏎ run again` (the launcher, on its item or topic, task and model) · `t transcript` · `del dismiss` |
| a kept run | `⏎ retry removal` · `d details` · `del dismiss` |
| a folded group's heading | `⏎ unfold` |
| `+ new agent` | `⏎ new agent` |

`Enter` on a live run always writes to it; only the label changes. An action a row does not offer — `o` on a
topic, `b` once the worktree is gone — is not listed. A done or finished run's box at the bottom of the Focus
lists the same actions. In the Focus the bar adds `esc back`; while the message box has the keys it reads `⏎
send · alt⏎ line · esc keep draft`.

`Ctrl+K` or `?` opens the command palette over the dashboard. It lists the selected row's actions first, then
the commands for the whole dashboard — new agent, filter the agents, refresh the lists, install the missing
plugins, show the last 24 hours — then every other action, dimmed, with the reason it does not apply now
(`end · no run selected`); an asking run's quick replies are listed by their labels. Each shows its key, or `palette only`. Typing filters it fuzzily (`l24` finds "show
the last 24 hours"), the matched letters underlined; `↑` `↓` choose; `Enter` runs the command exactly as its
key would; `Esc` closes it and keeps what you typed for the next time. Refreshing the lists and installing the
plugins have no key: they are the palette's. "Show the last 24 hours" opens a popup that charts, an hour a bar,
the agents active, the tokens they wrote and the runs started and ended; `Esc` closes it.

The layout follows the tab's width:

| Width | Fleet has the keys | Focus has the keys |
| --- | --- | --- |
| 100 columns or more | both, side by side | both, side by side (a leftover worktree's Focus takes the whole width) |
| 60 to 99 | the Fleet, the Focus below it at 60 % of the height | the Focus alone |
| below 60 | the Fleet alone | the Focus alone |

In a small tab, so that the Focus's tab keeps at least eight rows, the furniture gives way in this order: the
header's second line; `latest` while the Board tab is open; the top bar, whose gauge and pill move to the right
end of the action bar; and the tab strip, whose active tab moves into the header (`· board`). A row is cut
with `…` where it is too wide, never in the middle of a wide character. The details wrap; a command line or a
path among them is cut instead, never wrapped, so that what is shown is never broken where a space is.

The Fleet's groups fold with `z` into their heading, which is then selected; `z` or Enter on a folded heading
unfolds it, and Finished unfolds beyond its newest five the same way. The folds are kept across restarts.

A worktree no run accounts for — on an `agent/…` branch or under `worktree_root` — is listed under LEFTOVER
WORKTREES with its branch, or `detached`: a run that ended without removing its worktree leaves one, as does a
worktree made by hand under the root. One a run has just removed is not listed while the last listing predates
the removal; the repository's worktrees are listed again at once. The orchestrator never removes one: its
Details carry the `git -C <checkout> worktree remove <path>` line to run by hand from anywhere, each path
quoted for the shell (a path that would not show as it is, such as one holding a zero-width character, gets no
line), and git refuses it while it holds changes.

The dashboard is drawn in the design's palette, 256-colour by default: muted and faint text, an accent for
the place that has the keys, and a colour per state (asking, error, working, done), each paired with its own
glyph so that nothing is told apart by hue alone. Ordinary text and the background stay your terminal's own.

**The launcher.** `n` opens one popup to start any agent. With nothing typed it lists your pull requests
whose checks fail or whose reviewer asked for changes, then your other pull requests, then the issues
without one, each with its checks, review and whether an agent works on it (`⬢`). Typing filters them by
number and title, across every repository. While anything is typed, a row starts a **topic** on exactly
that text; a query that names an item exactly (`2375`, `#2375`, its URL or a Jira key) puts that item
first. `↑` `↓` choose a row and `Enter` starts the agent.

Under the query, the scope narrows the list: `repo  all  contour  pace …` and `show  all  needs attention  pull
requests  issues`. On the list, `←` `→` step the repository and `Ctrl+←` `Ctrl+→` the kind; a click on a chip
picks it. A topic starts in the repository the list is narrowed to, and the launcher opens again as you left it.
While every repository is listed, each row names its own. In a wide tab the launcher takes three quarters of the
width, from 80 to 160 columns.

Below the list are the settings for the chosen row: a topic's repository, the task (one that applies to
the item), the model, the permission profile, and an item's optional note, which is added to the prompt.
`Tab` moves into them and through them, `←` `→` change one, and typing into the note fills it. The query, the
note, the message box, the filter and the palette all take `Backspace`, `Alt+Backspace` (a word), `Ctrl+W` (back
to the last blank) and `Ctrl+U` (everything). Each
follows the chosen row, so the defaults are the task's own. The checkout and the worktree are shown
below them; `Ctrl+P` also shows the prompt and the agent's exact command line. `Esc` closes the
launcher. A dispatch the engine would refuse, an item an agent already works on for instance, says why
instead of starting. While Claude Code's status hooks or a plugin are missing the launcher says so, and
asks for a second `Enter`.

A task's `key` is still read, and may not be one of the dashboard's reserved keys (the configuration is
refused with a line that names them), but it no longer acts on the dashboard: the items it applied to are
the launcher's now.

**Topics.** Not everything an agent should do has a pull request or an issue. A topic's task is a `tasks`
entry with `applies_to: topic`, whose prompt takes `{topic}` for the text. Without one, a built-in `topic`
task makes the text the whole prompt. The run's id, branch and worktree are named from a slug of the text,
at most 40 characters of `[a-z0-9-]`, such as `agent/topic-investigate-the-flaky-test-…`. The text itself
reaches only the prompt and the dashboard. It starts on a new branch from the repository's base, as an
issue's run does. `Enter` on a finished topic run opens the launcher again on its text.

**The agents' skills.** A task's prompt such as `/contour-workflows:fix-ci {url}` names a skill, and
the skill comes with a Claude Code plugin. If that plugin is missing, the agent answers "Unknown skill".
`plugins` lists the plugins the agents need (by default `contour-workflows@contour-terminal`). With the status
hooks check done, the orchestrator asks the `claude` its panes run (`claude plugin list --json`) which
plugins are installed and enabled. Each missing one is a problem line, and the launcher warns of it and
asks for a second `Enter`. The palette's "install the missing plugins" (`Ctrl+K`, then `install`) runs
`claude plugin marketplace add` and `claude plugin install` into your own Claude Code and checks again. On a machine you reach only through `contour client`,
this is how its agents get their skills. The orchestrator never edits Claude Code's settings files.

## Attention

The dashboard tells you where to look without being asked:

- **A run that starts to need you** — it asks, waits for you, or stopped by an error — moves to Needs you
  without taking the selection, and the pill counts it. The action bar's right end names it,
  `pr-2375 needs you · home`, until you select it or it no longer needs you. A run that already waited when
  the dashboard opened is not named; one that goes from asking to waiting (as `3` leaves it) is not named again.
- **In another tab**, the orchestrator's status-line summary (`{DaemonExtension:orchestrator}`) counts the
  same runs: `⬢ agents 3/4 · ●2 need you`. A served tab can set no title and raise no notification or bell
  (the host drops those sequences), so the summary is the way the count reaches you elsewhere.
- **Unseen news:** a run that changed since you last selected it — its state, or its transcript as its
  statistics say — carries a `•` after its name, gone once you select it. In the Focus, the Board or the
  Transcript tab carries a `•` when its board or its last page changed since that tab was last shown. What the
  dashboard has seen is kept in memory: a dashboard that opens marks nothing.
- **Spinners:** a working run's glyph turns (`⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏`), a frame on each redraw the dashboard makes
  anyway — an age moving on, a followed transcript, a key — so it never wakes the guest on its own.

## Permissions

Every agent starts with a permission profile, which the orchestrator passes to Claude Code as `--settings`, so
routine work does not wait for someone who may be on another machine. The default, `standard`, lets an agent
edit its worktree, build, test, use local `git` and read the forge without asking; pushing, opening a pull
request and anything not listed still ask, and such a run shows as asking. It denies `sudo`, `rm -rf`, a forced
push and `git reset --hard`. `trusted` adds pushing and opening a pull request, for a task that publishes its
own fix (`permissions: trusted` on the task). `permissions: ask` gives every decision back to Claude Code.
`permission_profiles` adds profiles or replaces the built-in ones; `bypassPermissions` is refused, so a full
bypass is never one line of configuration away.

A run keeps the profile it started with, and its details name it (`perms  standard · 19 allowed · 4 denied`).
A changed configuration applies to the next run, not to a running one. The orchestrator never writes Claude
Code's settings files: the profile lives only in the agent's command line, which the consent dialog shows.

## Boards and transcripts

Both are tabs of the Focus, and both are read-only.

**The board** is the agent's `STATUS.md` at its worktree's root, read with `file-read` from the `worktrees`
root and shown with how long ago it was written, again every five seconds while its run is selected. Every
agent is asked to keep one: its system prompt (`--append-system-prompt`) carries `status_board`, by default an
instruction to keep `STATUS.md` with the `contour-workflows:status-board` skill. The skill publishes the board
as a private claude.ai page; the agent is told never to open it — no browser window per agent — and to put the
page's link in `STATUS.md` as a `**Page:** <link>` line under its title. The Board tab's first line then ends in
`open page`, a link that opens the page on your machine (only a `https://claude.ai/` link is taken, since a board
is untrusted text). A hook's request proved
unreliable; a system prompt holds for the whole session. `status_board: off` asks for none. Once an agent
has run for `status_board_after` the engine looks for the file. A board is untrusted text: drawn inert, cut at
64 KiB, and refused when it leads outside the worktree. Ending a run removes an untracked `STATUS.md` (`git
clean -f -- STATUS.md`) before the worktree is checked for work, so a board never keeps a worktree. The Board
tab is offered on a finished run only while its worktree was kept, and such a board is read once, not every
five seconds.

A board is Markdown, and it is drawn that way: headings in the accent, `•` for list items, `☐`/`☑` for task
boxes, `│` for quotes, aligned tables, and dim code blocks. Inline markers are dropped, and a link shows only
its text; a bare URL is a hyperlink. Its task boxes are the run's `▰▱` meter in the Fleet, and its first open
box the row's second line.

**The transcript** is the run's Claude Code session log, a page of 30 entries at a time, read with the
declared command `agent-transcript` (`contour agent transcript … --format 1 --json`): when the run is
selected, at once when the log has grown a page, and every five seconds while the agent runs and the tab
follows it — one read per run at a time. The host finds the log by the run's session id and binds it to the
run's worktree, which need not exist any more, so a finished run keeps its transcript. The page's JSON is a
contract (`contract/agent-transcript/`): a page in another format, or Contour's refusal of the one asked for,
shows "format mismatch: update contour or the orchestrator", never a parse error. A read that fails shows the
command to run by hand.

Its screens are pinned by golden files in `view/testdata/`, one per scene and size; after a deliberate
change, `ORCHESTRATOR_UPDATE_GOLDENS=1 orchestrator_test "[view]"` writes them again, to be checked by eye.

## Messages

On a machine you reach only through `contour client`, the dashboard is the only place you can talk to an
agent. `Enter` on a live run gives its message box the keys: every key you press goes to the box, `Enter`
sends, `Alt+Enter` adds a line, a paste keeps its line feeds, and `Esc` goes back to the Fleet keeping the
draft. The text goes through the host's `pane-send` (the manifest declares `pane-input`, and the consent dialog
asks for it): it is typed into the agent's tab as one bracketed paste followed by Enter, so Claude Code
receives it as one message. An agent at a permission prompt has no message box: its menu takes the quick
replies (below). After a send the draft clears, `latest` shows it (`you · sent 14:31`) until the transcript does, the
action bar says `sent to pr-2375`, and the keys return to the Fleet. The orchestrator types nothing else, ever
— but the quick reply you press — and reads no screen: the Transcript tab shows what the agent did. A message the host refuses, such as one
holding a control character, or one sent after the agent's tab closed, comes back to the box with the keys,
and the action bar says why.

## Quick replies

While an agent asks for a permission, Claude Code shows a select menu, and the Focus shows the **asking box**
in the message box's place: the request, as the status hook reported it (`Bash: ctest -R reflow`), and the
reply chips, `1 yes   2 yes, don't ask again   3 no, and say why`. The action bar opens with `1 2 3 answer`.
Pressing a digit in the Fleet or in the Focus answers the menu without opening the agent's tab: the host's
`pane-key` presses that one key in it, with no paste and no Enter, as a menu needs (a pasted digit is
ignored). The chip fills and the box dims (`answered yes`) until the agent stops asking; still asking 10 s
later, the action bar says `pr-2375 still asks · r opens its tab` and the chips are live again.

- **Only what you can see.** A digit answers only while the selected run asks, its request then on screen in
  every band: the asking box, or in a narrow tab the row's second line. Otherwise the bar says `select the
  request first`. An asking run has no message box, so a digit never has to choose between answering and
  typing; in the message box of any other run it types.
- **`3` waits for your reason.** It presses Escape, which rejects the request on every Claude Code menu
  (No is option 2, 3 or 4 depending on the menu), turns the run waiting, and opens its message box: the next
  keys say why.
- **The labels are configuration**, never read from the tab: `quick_replies` (see `docs/extensions.md`)
  names each digit's label, the key it presses and whether the message box follows. A changed Claude Code
  menu is fixed there. The defaults follow the menu of a Bash or edit prompt; on a menu with two options,
  `2` is No.
- **A question is not a menu.** An agent that asks a question, or a wait the hook cannot name, is waiting, not
  asking: it shows no chips, and you answer it with a message or in its tab (`r`).
- A reply on a run that stopped asking meanwhile is not pressed (`pr-2375 is no longer asking`), since a digit
  where no menu is up would be typed into the agent's prompt.

## Statistics

A run's details say what its agent spent and did: its wall time from dispatch to end, the time Claude Code
measured it working, and the time it waited for you (blocked); its tokens by kind and how many were read from
the cache; the cost Claude Code recorded when the agent exited, as it reports it; its API calls, prompts and
failed calls; the tools it called most and how many failed; its subagents and compactions; and the pull
requests it opened. The numbers are read with the declared command `agent-stats` (`contour agent stats …
--format 1 --json`, contract in `contract/agent-stats/`): a minute after the agent starts and every minute
after, and once more when its run finishes. They are stored with the run, beside the history of the states it
entered and when, so a finished run keeps them after its worktree and Claude Code's log are gone.

Once a run finished and its last statistics were read, it is appended to `agent-runs.jsonl` in the daemon's
state directory (`~/.local/state/contour` on Linux), one JSON line per run, for analysis with `jq`, DuckDB or
pandas: see `contract/agent-runs/`.

## The contract

What the extension and Contour must agree on comes in three parts, each tested from both sides:

- **`contract/manifest-v2/`**: the manifest's line grammar as a corpus of valid and invalid manifests, the
  extension's own manifest among them, with the consent text Contour renders for it.
- **`contract/agent-transcript/`**: the JSON pages `contour agent transcript` prints, format 1, each beside the
  log it was printed from.
- **The `terminal-extensions:daemon` WIT world**, at `wit/daemon/` in Contour's tree: Contour's public
  interface, from which `guest/bindings/` is generated.

`contract/README.md` says how each is checked.

## Building

The project builds inside Contour, whenever `CONTOUR_EXTENSIONS` is on, and takes every dependency from
that tree: nothing is fetched. Its tests build with `ORCHESTRATOR_TESTING`, which follows `CONTOUR_TESTING`:

    cmake --build --preset clang-asan --target orchestrator_test
    out/clang-asan/extensions/orchestrator/orchestrator_test

It includes nothing from Contour's `src/`, so that it can move into a repository of its own. Sources
include each other as `<orchestrator/core/X.hpp>`, so a standalone checkout must be a directory named
`orchestrator`. Configured on its own, it fetches glaze, libunicode and core-cpp at the versions Contour
carries, and Catch2 too with `-DORCHESTRATOR_TESTING=ON`.
