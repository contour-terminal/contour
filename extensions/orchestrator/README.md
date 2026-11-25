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
NTSTATUS — is shown in hex (`it exited with 0xC0000005`) when its code is what is shown: a command that wrote
nothing to stderr.

## The dashboard

The orchestrator draws its own tab (`view/`, drawn with core-cpp's headless terminal UI). It has two views, which
`w` switches between, or a click on a view's name in the top bar:

- **Agents**, where you see, read and talk to agents: the **Fleet** on the left lists your runs, and the
  **Focus** on the right shows the one selected. The dashboard opens on it.
- **Work**, where you see what needs doing: your open merge requests and the tickets you work on, grouped by
  what they need from you, and where you start an agent on one (see "The Work view" below).

`n` starts a custom task in either view (see "`n`: a custom task" below).

A top bar names the two views (`✦ Agents  Work`, the one shown lit) and carries the fleet gauge (`⬢ 3/4 ▰▰▰▱`,
active runs of `max_agents`), a `● 2 need you` pill while a run waits for you, and `plugins missing · ^K install`
while a plugin the tasks need is missing. Problems — a failing `gh`, a list cut at its limit, a configuration that does not parse — sit
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
below). `+ new task` is the last row; with no run it is the only one, and the Focus shows a primer. Every
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

**The mouse.** The dashboard asks for the mouse (SGR reporting, DECSET 1000 and 1006). In the Agents view a
click selects a row of the Fleet, and a double click runs its primary action; one click on `+ new task` opens the
`n` box. A click switches the Focus's tab, answers with a quick-reply chip, gives the message box the keys, opens
a link, or runs a palette entry. A click on a view's name in the top bar shows that view. A click beside an open
popup closes it. The wheel moves the Fleet's selection, scrolls the Focus's tab, or moves a popup's highlight,
whichever is under the pointer. While the `n` box's text or the dialog's note is typed, the wheel does nothing,
even over a setting; on the settings it moves between them or steps the one under the pointer. Each frame records
what it drew where, so a click lands on what you see.
Shift+drag still selects text in Contour. The Work view's mouse is described with it, below.

**Places and keys.** The Fleet has the keys by default, and its letters are commands; the Focus gets them with
`Enter` on a live run or `Tab` on any, and its border lights up. In the Focus of a live run the message box
has the keys: every printable key types, digits too, so no letter is ever both a command and text.

| Key | In the Fleet |
| --- | --- |
| `↑` `↓` `j` `k` | move the selection |
| `Home` `End` `gg` `G` | select the first or the last row |
| `Ctrl+D` `Ctrl+U` | move the selection by half the rows shown |
| `Enter` | the selected row's primary action, which the action bar lists first (below) |
| `Tab` | give the Focus the keys |
| `b` `t` `d` | show the Focus's Board, Transcript or Details tab |
| `PgUp` `PgDn` | scroll the Focus's tab |
| `←` `→` `h` `l` | turn the transcript's pages; on the Board or Details, or past the transcript's first or last page, show the tab beside |
| `n` | start a custom task: the `n` box (below) |
| `w` | show the Work view |
| `/` | filter the runs by text, Finished's folded ones too; `Enter` or `Esc` stops typing and keeps the filter, and `/` types it again |
| `Ctrl+K` `?` | open the command palette: every action, and the commands that have no key |
| `e` | end the selected run; asks once (`e again to end · esc keeps it`) |
| `r` | show the selected agent's tab |
| `o` | open the run's pull request or issue on your machine (see "Links" above) |
| `x` | check a kept worktree again |
| `Del` | drop a finished run from the list |
| `z` | fold the selected run's group, or unfold a folded one |
| `1` `2` `3` | answer the selected agent that asks, with that quick reply (below) |
| `Esc` | drop the filter (after the `Esc` that stopped typing it) |

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
| `Esc` `Tab` | back to the Fleet, keeping the draft (drafts are kept per run, in memory); every letter, `j` `k` `q` included, types |
| `Ctrl+K` | open the command palette |

In the Focus of a run without a message box — one finished, preparing or ending, or a leftover worktree — the
Fleet's letters act, and `↑` `↓` `j` `k` scroll a line instead of moving, `Ctrl+D` `Ctrl+U` half the tab,
`Home` `End` `gg` `G` to its top or end. `Esc` or `Tab` goes back.

**Actions and the palette.** What the dashboard offers follows the selected row. Each row's state has its
actions in rank order; the first is what `Enter` does, and the action bar lists them while they fit, then
`n new task` and `^k all actions`:

| Selected | Action bar |
| --- | --- |
| a run that asks | `1 2 3 answer` · `⏎ see request` · `r agent tab` · `e end` |
| a run that waits, or stopped by an error | `⏎ reply` · `t transcript` · `b board` (or `d details`) · `e end` |
| a run starting or at work | `⏎ message` · `t transcript` · `b board` · `e end` (starting: `r agent tab`) |
| a run that reported done | `⏎ follow up` · `o open link` · `t transcript` · `e end` |
| a finished run | `⏎ run again` (the start dialog on its item and task, or the `n` box on its topic's text) · `t transcript` · `del dismiss` |
| a kept run | `⏎ retry removal` · `d details` · `del dismiss` |
| a folded group's heading | `⏎ unfold` |
| `+ new task` | `⏎ new task` |

`Enter` on a live run always writes to it; only the label changes. An action a row does not offer — `o` on a
topic, `b` once the worktree is gone — is not listed. A done or finished run's box at the bottom of the Focus
lists the same actions. In the Focus the bar adds `esc back`; while the message box has the keys it reads `⏎
send · alt⏎ line · esc keep draft`.

`Ctrl+K` or `?` opens the command palette over the dashboard. It lists the selected row's actions first, then
the commands for the whole dashboard — new task, filter the agents, refresh the lists, install the missing
plugins, show the last 24 hours, switch view, fold the other groups (the Work view's) — then every other action, dimmed, with the reason it does not apply now
(`end · no run selected`); an asking run's quick replies are listed by their labels. Each shows its key, or `palette only`. The palette always types
its query: every letter, `j` and `k` included, filters it fuzzily (`l24` finds "show the last 24 hours"), the
matched letters underlined; `↑` `↓` or `Ctrl+N` `Ctrl+P` choose; `Enter` runs the command exactly as its key
would; `Esc` closes it at once and keeps what you typed for the next time. Refreshing the lists and installing the
plugins have no key: they are the palette's. "Show the last 24 hours" opens a popup that charts, an hour a bar,
the agents active, the tokens they wrote and the runs started and ended; `Esc` or `q` closes it.

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

## The Work view

`w` shows it: your open merge requests (pull requests, on GitHub) and the tickets you work on — the Jira work
items and forge issues your repositories' `issues` searches list — one row each, across every repository, in
five groups shown in this order:

| Group | Holds |
| --- | --- |
| Needs you | a merge request whose pipeline failed, whose reviewer asked for changes, or that conflicts with its base |
| Ready to merge | a merge request whose pipeline passed (or that has none), without conflicts, and approved or needing no review (on GitLab: `mergeable`) |
| In review | every other open merge request: its pipeline running, its review pending |
| No MR yet | a ticket in progress that no merge request names |
| Everything else | a draft merge request, whatever its pipeline or conflicts, and a ticket not in progress; folded by default (`work.folded`) |

A draft is checked first; then the first group whose rule matches takes the item. A ticket a merge request names
is not listed on its own: its status shows on the merge request's row. A merge request names a Jira ticket with
the first Jira key in its title, else in its source branch, of a project a Jira source covers — a project one of
its tickets is in, or one its JQL names (`project = PRJ`, `project in (PRJ, ABC)`) — so that a stray `UTF-8`
names nothing; it names a forge issue with the forge's own closing link or a `#<number>` in its title. A Jira
ticket is in progress when Jira's status category is *In Progress*, or, with `in_progress_jql`, when that query
lists it (see "Configuration" below). A forge issue counts as in progress, since its search already selects your
work, and shows `● open`. A ticket two repositories list belongs to the first in configuration order, which is
where its run checks it out.

Each row has these columns:

| Column | A merge request's row | A ticket's row |
| --- | --- | --- |
| MR | `!812` (GitLab) or `#2375` (GitHub) | — |
| Ticket | the key of the ticket it names, or `·` | its key |
| Title | its title, cut with `…` | its summary |
| Pipeline | `✗ pipeline`, `◌ running`, `✓ pipeline` or `·` | `·` |
| Review | `⚠ conflicts` (which wins), `⟲ changes`, `✓ approved`, `… review` or `· no review` | `·` |
| Jira | `◆ <status>` of its ticket, `◆ ?` while its status is unknown, or `·` | `◆ <status>`, or `● open` for a forge issue |
| Agent | `⬢ <worker>` while a run works on it, else how long ago it was updated | the same, but a ticket has no age |

As the tab narrows, a row drops the age first, then shows the review and then the pipeline by their glyph alone;
below 60 columns the Ticket column goes too. Within a group the merge request updated last comes first. A
ticket has no update time — `acli`'s search cannot print a Jira ticket's, and a forge issue's is not read — so
tickets follow the merge requests in the order their search returned them: end a JQL with `ORDER BY updated
DESC` to list the newest first.

Above the list is the filter bar: `/ filter`, the repository chip (`‹ all repositories ›`, or one repository) and
the sort chip (`sort: importance`, or `sort: last update`, one flat list without groups). The list scrolls with
the selection kept in view, and a scrollbar shows on its right edge while it does not fit. Below it, the details
line says what the selected row is about: its failing checks (`pipeline fails: test-windows`), the base it
conflicts with, its ticket and that ticket's status, and the agent on it (`#2375 · contour · pipeline fails:
test-windows · PRJ-1234 In Progress`). A group starts folded or open as `work.folded` says (see "Configuration"
below); a group you fold or unfold stays as you left it, over whatever `work.folded` says then, and is kept across
restarts, as the sort is; the filter and the repository chip are not. A group with nothing to show has no heading,
unless `work.fold_empty` is `false`: then every group shows its heading, an empty one with `0`.

| Key | In the Work view |
| --- | --- |
| `↑` `↓` `j` `k` `PgUp` `PgDn` | move the selection |
| `Home` `End` `gg` `G` | select the first or the last line |
| `Ctrl+D` `Ctrl+U` | move the selection by half the lines shown |
| `Enter` | on a row, open the start dialog (below); on a group's heading, fold or unfold it |
| `z` | fold or unfold the selected line's group |
| `/` | filter: what you type matches a merge request's number (`!812`, `#2375`, `2375`), a ticket's key, a title, a repository or a URL, case-insensitively; `Enter` or `Esc` stops typing and keeps the filter, and `/` types it again |
| `Esc` | clear the filter (after the `Esc` that stopped typing it) |
| `s` | sort by importance (in groups) or by last update (one flat list) |
| `←` `→` `h` `l` | step the chip with the keys: on the sort chip, switch the sort; anywhere else, step the repository chip |
| `o` | open the selected merge request, or a ticket row's ticket, on your machine (see "Links" above) |
| `m` | open the row menu |
| `n` | start a custom task: the `n` box (below) |
| `w` | show the Agents view |
| `Tab` | move the keys along the list, the filter, the repository chip and the sort chip; `Enter` steps a chip that has them |
| `Ctrl+K` `?` | open the command palette; a command chosen there acts on the Work view, and "fold the other groups" folds every group but the selected line's |

The action bar lists the keys that apply to the selected line (`⏎ start · n new task · o open · m menu · z fold ·
/ filter · s sort · → repository · w agents · ^k all actions`, as many as fit), and a click on one runs it.

**The row menu** (`m`, or a right-click on a row) offers *Start agent…*, *Open MR*, *Open ticket* and *Fold
group*; an entry that does not apply to the row is dimmed. `↑` `↓` `j` `k` choose, `Home` `End` `gg` `G` go to the
first or last, `Enter` runs one, `Esc` or `q` closes it. It
offers no "copy branch name": a served tab drops OSC 52, so no copy would reach your clipboard.

**The mouse** in the Work view:

| Where | Click | Double click | Right-click | Wheel |
| --- | --- | --- | --- | --- |
| a row | select it | open the start dialog | the row menu | scroll the list three lines; the selection stays until it leaves the view |
| a merge request's number or a ticket's key | select the row | open it on your machine | as on the row | as on the row |
| a group's heading | fold or unfold it | nothing more: the first click did it | fold every other group | scroll |
| the scrollbar | jump there; hold the button on the thumb to drag it | — | — | scroll the list |
| the filter, the repository chip, the sort chip | type into the filter; step the chip | — | step the chip back | step the chip |
| a key in the action bar | run it | — | — | — |
| the start dialog or the `n` box | pick a setting (the note or the text: type it); on its value or its `‹ ›`, step it; pick a worker's chip | start (on a label, the note or a worker; a double click on `‹ ›` steps twice) | step it back | step the setting under the pointer |
| beside a dialog or the menu | close it | — | — | — |

While the Work view is shown the dashboard also asks for every move of the pointer (DECSET 1003): the row under
it is marked with a dim `▏`, redrawn only when the pointer reaches another row, and holding the button on the
scrollbar drags it. A served tab may set that mode, but whether a terminal passes the moves on has not been
checked in a live session: without them nothing is marked, the scrollbar takes clicks only, and everything else
works as described. Every mouse action has a key.

## Starting an agent

**The start dialog.** `Enter` or a double click on a row, or *Start agent…* in its menu, opens the dialog over
the board, with the item, then its settings:

- **Task**: suggested from the row, the first that matches — a failed pipeline suggests the task for fixing CI,
  a conflict the one for resolving conflicts, a reviewer's changes the one for addressing the review, a ticket
  the one for working on an issue; otherwise the first task that applies to the item. The reason is shown
  beside it (`pipeline fails: test-windows`, `conflicts with main`, `changes requested`, `ticket in progress`).
  A task's role says which of these it is (`role:`, see "Configuration" below); a rule whose role no task that
  applies to the item has falls through to the next. Only the tasks that apply to the item are offered,
  and choosing another brings its own model, permission profile and worker.
- **Worker**: every configured worker as a chip with the number of runs it has now (`claude-work 1`). It
  defaults to the task's own worker; while that worker already runs an agent, to the worker with the fewest
  runs (ties in configuration order). A worker whose setup is incomplete — Claude Code's status hooks or a
  plugin missing for its set — is marked `(setup)`, the dialog says what is missing, and starting on it asks for
  a second `Enter`. A worker that takes no permission profile for the task (a Copilot worker) is marked
  `(no profile)`, and Perms is passed over. The worker chosen holds for this run only.
- **Model**: the task's model, any of `models`.
- **Perms**: the permission profile: the task's, else the default (`auto`, unless `permissions` names another)
- **Note**: optional text added to the prompt; it may not start with `-`.

Below them are the checkout and the worktree; `Ctrl+P` also shows the prompt and the agent's exact command line.
The dialog opens on its settings, Task selected. `j` `k` (or `↑` `↓`, `Tab` `Shift+Tab`) move between them, and
`gg` `G` (or `Home` `End`) go to the first and the last; `h` `l` (or `←` `→`) change the one selected. No letter
types there. `i` types the note, as `Tab` onto it or a click on it does; while it is typed every letter is text,
`j`, `k` and `q` included, and `Esc` leaves the note for the settings. `Enter` on the Note line types the note;
anywhere else, and while the note is typed, it starts the agent. A second `Esc`, or `q`, closes the dialog. While
an agent already works on the item the dialog says so (`⬢ claude-work already works on this`) with the engine's
refusal, and `Enter` starts nothing; so does any other dispatch the engine would refuse. `⏎ run again` on a
finished run opens the dialog on the same item and task, while the Work view still lists the item.

The `n` box's text, the dialog's note, the message box, the filters and the palette all take `Backspace`,
`Alt+Backspace` (a word), `Ctrl+W` (back to the last blank) and `Ctrl+U` (everything). A paste into the box's
text or the note arrives as one line of text; the message box keeps a paste's line feeds. On the settings of the
box or the dialog, where there is no text to edit, these keys do nothing.

A task's `key` is still read, and may not be one of the dashboard's reserved keys (the configuration is refused
with a line that names them), but it starts nothing: an item's agent starts from the Work view.

**`n`: a custom task.** `n`, in either view, or `+ new task`, opens a box that asks "What should the agent do?",
with the keys in its text. `Enter` starts the agent at once, on these defaults: the **repository** the Work view's
chip names, else the selected row's, else the first configured; the **worker** of the topic task, by the same
busy rule as the dialog's; and the topic task's **model**. The box opens typing its text, so typing and then
`Enter` still starts at once. `Esc` goes to its settings, Repo selected, where `j` `k` (or `↑` `↓`) move and `h`
`l` (or `←` `→`) change one; `Tab` and a click reach them too. `i`, or `Enter` on the text, types the text again;
`Enter` elsewhere starts. A second `Esc`, or `q`, closes the box. An empty text starts nothing, and a worker whose
setup is incomplete asks for a second `Enter`, as in the dialog. `⏎ run again` on a finished topic run opens the
box again on its text.

The search launcher that `n` opened before is gone. Its jobs moved: browsing the pull requests and issues to the
Work view; a query naming one item (`#2375`, its URL, `PRJ-1234`) to the Work view's filter; typing a topic to
the `n` box.

**Preparing a run.** A started agent's run is prepared before its tab opens: the remote is fetched, the
worktree added (on a new branch for an issue or a topic, detached for a pull or merge request), and a pull or merge
request checked out there (`gh pr checkout`, `glab mr checkout`). Each of these steps writes refs that every
worktree of the repository shares, so two of them at once fail on each other's lock (`cannot lock ref`). A
repository therefore runs one such step at a time: a second run started meanwhile waits, shown as preparing, and
takes its turn when the step ahead of it answers. Runs of different repositories prepare side by side. The turn
is kept per configured repository path: two configured repositories that are worktrees of one git repository, or
the same directory spelled two ways, take their turns separately and can still meet on a ref. A step that
still fails with `cannot lock ref` — another git, such as an IDE's background fetch, held the ref — is tried once
more two seconds later; a second such failure, or any other, ends the run as `preparation-failed`.

**Topics.** Not everything an agent should do has a pull request or an issue. A topic's task is a `tasks`
entry with `applies_to: topic`, whose prompt takes `{topic}` for the text. Without one, a built-in `topic`
task makes the text the whole prompt. The run's id, branch and worktree are named from a slug of the text,
at most 40 characters of `[a-z0-9-]`, such as `agent/topic-investigate-the-flaky-test-…`. The text itself
reaches only the prompt and the dashboard. It starts on a new branch from the repository's base, as an
issue's run does.

**The item's key.** `{key}` in a task's prompt stands for the item's key: a Jira key such as `PRJ-1234`, or the number of a forge issue, pull request or merge request. A prompt may use `{url}`, `{key}` or both; a topic task's prompt may not use `{key}`.

**The agents' skills.** A task's prompt such as `/contour-workflows:fix-ci {url}` names a skill, and
the skill comes with a Claude Code plugin. If that plugin is missing, the agent answers "Unknown skill".
`plugins` lists the plugins the agents need (by default `contour-workflows@contour-terminal`). With the status
hooks check done, the orchestrator asks the `claude` its panes run (`claude plugin list --json`) which
plugins are installed and enabled. Each missing one is a problem line, and the start dialog and the `n` box
warn of it and ask for a second `Enter`. The palette's "install the missing plugins" (`Ctrl+K`, then `install`) runs
`claude plugin marketplace add` and `claude plugin install` into your own Claude Code and checks again. On a machine you reach only through `contour client`,
this is how its agents get their skills. The orchestrator never edits Claude Code's settings files.

## Configuration

The whole configuration is in [Configuring the orchestrator](../../docs/extensions.md#configuring-the-orchestrator).
The Work view adds optional keys; every configuration that was valid before stays valid.

```yaml
repos:
    - path: ~/repo/project
      forge: gitlab
      issues:
          - source: jira
            site: https://example.atlassian.net
            jql: 'assignee = currentUser() AND resolution = Unresolved ORDER BY updated DESC'
            in_progress_jql: 'status in ("In Progress", "Selected for Development")'   # optional
tasks:
    - { id: fix, key: f, label: fix CI, applies_to: pr, model: sonnet, role: fix_ci,
        prompt: "/contour-workflows:fix-ci {url}" }
```

- **`in_progress_jql`** (a Jira source's, optional): a ticket is in progress when it matches
  `(<jql>) AND (<in_progress_jql>)`, a query `acli` runs once per poll; together the two must fit one argument.
  Without it a ticket is in progress when Jira's status category is *In Progress*, whatever the project calls the
  status. A failed query keeps that rule for the poll and shows a problem line.
- **`role`** (a task's, optional): `fix_ci`, `resolve_conflicts`, `address_review` or `work_on_issue`, the state
  the start dialog suggests the task for. Without it, a task whose `id` is one of these has that role, so
  `id: fix_ci` needs none. Any other value is refused, and the line names the four.

**`work`** (optional) sets how the Work view starts:

```yaml
work:
    folded: [everything_else]   # the default
    fold_empty: true            # the default
```

- **`folded`**: the groups shown folded until you unfold them, by the names `needs_you`, `ready_to_merge`,
  `in_review`, `no_mr_yet` and `everything_else`; the default is `[everything_else]`, and `[]` opens every group.
  A group you folded or unfolded yourself keeps your choice, whatever the list says.
- **`fold_empty`**: `true`, the default, hides a group with nothing in it; `false` shows every group's heading.

**`keys`** (optional) rebinds the navigation keys of the lists and the dialogs:

```yaml
keys:
    vim: false              # arrows, Home/End, Esc and Enter only
    move_down: [down, "^n"] # a list replaces the action's defaults
```

`vim` (default `true`) adds the vim keys to each action's plain ones; `vim: false` keeps the plain keys alone.
Each action takes a list of keys, and a list replaces that action's defaults whole, whatever `vim` says. The
defaults, plain then vim (the action bar names an action by its first key):

| Action | Defaults | What it does |
| --- | --- | --- |
| `move_up` | `up` `k` | the row or line above; in the Focus, scroll up a line; a dialog's setting above; the row menu's entry above |
| `move_down` | `down` `j` | the row or line below; in the Focus, scroll down a line; a dialog's setting below; the row menu's entry below |
| `first` | `home` `gg` | the first row or line; the Focus's top; a dialog's first setting; the row menu's first entry |
| `last` | `end` `G` | the last row or line; the Focus's end, live; a dialog's last setting; the row menu's last entry |
| `half_page_up` | `^u` | half the rows shown up; in the Focus, scroll half the tab |
| `half_page_down` | `^d` | half the rows shown down; in the Focus, scroll half the tab |
| `previous_value` | `left` `h` | the transcript's previous page; the Work view's chip back; a dialog's setting back |
| `next_value` | `right` `l` | the transcript's next page; the Work view's chip on; a dialog's setting on |
| `leave_field` | `esc` | stop typing a text field (acts only while one is typed): a filter keeps its text, the message box returns to the Fleet with the draft kept, the `n` box goes to Repo |
| `enter_field` | `enter` `i` | type a dialog's text field (`⏎` types only on the field's line, and stays a list row's own key) |
| `close` | `esc` `q` | close what is open: the last 24 hours, the row menu, or the start dialog and the `n` box on their settings; `Esc` still closes each of them when `close` is rebound |

A key is a printable character, `^` and a letter for Ctrl (not `^h`, `^i`, `^j` or `^m`, which terminals send as
Backspace, Tab, LF and CR), two characters such as `gg`, or one of `up down left right home end pgup pgdn enter
esc tab del`. Quote a key YAML reads as something else: `"^n"`, `"?"`, `"1"`. A key may act only once where it
acts: one that another action, or one of the dashboard's own commands (`e`, `/`, `w`, the digits, ...), already
holds is refused, and so is a two-character key whose first character is bound alone, or the reverse. The
problem line names the other holder:

    keys.close[0]: is already bound: j, also keys.move_down

A task's `key` is not a navigation key's concern: it starts nothing from a list, so a task keyed `G`, `q` or `l`
still loads, and the navigation key wins in the lists and in the start dialog, where a task's key picks no task.
The keys take effect in the Fleet, the Focus, the Work list, the row menu, the last 24 hours, the start dialog
and the `n` box, and `leave_field` in the filters and the message box. The palette consults none of them: it
always types its query, and only `↑` `↓` `Ctrl+N` `Ctrl+P` choose there. While a text field is typed, a letter or
a chord never acts as navigation; the plain special keys (`↑` `↓` `Home` `End`) still move between a dialog's
settings, and from a filter between the rows (the message box keeps them). A key with `Alt` is how `Esc` reads when the next key follows it at once, so while a field is typed
it does what `Esc` does there, and with `Esc` rebound away from `leave_field` it does nothing.

**GitLab.** `glab mr list` carries no pipeline and no approvals, so per GitLab repository and poll the orchestrator
also asks for the project's newest pipelines and its failed jobs, two REST calls through `glab api`:

    glab api projects/:id/pipelines?per_page=100&order_by=id&sort=desc
    glab api projects/:id/jobs?scope[]=failed&per_page=100

A pipeline belongs to a merge request when it ran on the merge request's head commit, or for its
`refs/merge-requests/<iid>/head` or `refs/merge-requests/<iid>/merge` (a merged-results pipeline, whose commit is
not the head); of a merge request's pipelines the newest wins, and its failed jobs are the failing checks the
details line and the start dialog name. Approval still comes from `detailed_merge_status`, which says
`not_approved`, `requested_changes` or `mergeable`, so a merge request is Ready to merge only while it says
`mergeable`, however its pipeline did: any other status (`discussions_not_resolved`, `need_rebase`,
`blocked_status`, `checking`, …) names no review, so the row stays In review and shows `… review`, not
`· no review`. A merge request none of the listed pipelines matches keeps what `detailed_merge_status` alone
says, and its details line says `pipeline and review inferred from the merge status`. A pipeline call that fails leaves every merge request
so, with a problem line (`contour: the pipeline list could not be read; CI and review are inferred from GitLab's
merge status`); a failed job list leaves the failing checks unnamed, also with a line. It is never one call per
merge request.

**Jira.** The search asks for `--fields key,summary,status,issuetype`, so each ticket comes with its status and
status category; `acli` allows no `updated` among them, so a ticket shows no age and keeps the order its JQL
gives. A merge request may name a ticket no search listed — someone else's, or one outside the JQL: all of
those are read with one `key in (PRJ-1234, …)` search per poll, so that every merge request shows its ticket's
status. Keys are checked against the key pattern before they reach the command; when
that lookup fails, those merge requests show without their ticket, with a problem line. Each Jira source with
`in_progress_jql` adds its one query per poll.

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
routine work does not wait for someone who may be on another machine. The default, `auto`, runs an agent in
Claude Code's auto mode on top of `standard`'s rules: Claude Code decides what needs approval, and the four denies
still hold. The denies are `sudo`, `rm -rf`, a forced push and `git reset --hard`. `standard` lets an agent edit
its worktree, build, test, use local `git` and read the forge without asking; pushing, opening a pull request and
anything not listed still ask, and such a run shows as asking. `trusted` adds pushing and opening a pull request,
for a task that publishes its own fix. `ask` gives every decision back to Claude Code. Any of them is selectable
per task (`permissions:` on the task), as the default (`permissions: standard` at the top level), and in the start
dialog's Perms. If your Claude Code cannot run an agent in auto mode, it decides how the agent runs; set
`permissions: standard` to keep today's behaviour. `permission_profiles` adds profiles or replaces the built-in
ones; `bypassPermissions` is refused, so a full bypass is never one line of configuration away.

A run keeps the profile it started with, and its details name it (`perms  auto · 19 allowed · 4 denied`).
A changed configuration applies to the next run, not to a running one. The orchestrator never writes Claude
Code's settings files: the profile lives only in the agent's command line, which the consent dialog shows.

## Workers

A **worker** is who runs a task: an `id`, an `agent` (`claude` or `copilot`) and, optionally, an `env_set`, the
name of a variable set the user defines in the daemon's configuration (`daemon.extensions.orchestrator.env_sets`,
see [Configuring the orchestrator](../../docs/extensions.md#configuring-the-orchestrator)). The set's values come from that configuration, never from the orchestrator: it only
names the set, and the host fills in the values the consent dialog showed. A task picks its worker with
`worker:`; without one, the first worker runs it, and a task naming a worker that does not exist is a problem
line (`tasks[1].worker: names no worker; the first worker runs it: …`) and also runs on the first. Without
`workers` there is one implicit worker, `claude` (agent `claude`, no set), and everything runs as it did before
workers existed. A variable a worker's set does not hold is inherited from the daemon's environment, as any
other.

Two Claude Code accounts side by side, one for work and one personal, differ only by `CLAUDE_CONFIG_DIR`; a third
worker runs GitHub Copilot CLI:

```yaml
daemon:
    extensions:
        orchestrator:
            env_sets:
                claude-work:     { CLAUDE_CONFIG_DIR: 'C:\Users\me\.claude-work' }
                claude-personal: { CLAUDE_CONFIG_DIR: 'C:\Users\me\.claude-personal' }
            config:
                models: [sonnet, opus, auto]
                workers:
                    - { id: claude-work,     agent: claude,  env_set: claude-work }
                    - { id: claude-personal, agent: claude,  env_set: claude-personal }
                    - { id: copilot,         agent: copilot }
                tasks:
                    - { id: fix_ci, key: f, label: fix CI, applies_to: pr, model: sonnet,
                        worker: claude-work, prompt: "/contour-workflows:fix-ci {url}" }
                    - { id: chore, key: c, label: chore, applies_to: issue, model: auto,
                        worker: copilot, prompt: "Work on issue {key}: {url}" }
```

A worker's set reaches everything the orchestrator runs for it: the agent's tab, the transcript and statistics
reads, the status-hooks check and the plugin commands. A run keeps the worker and set it started with, also
when the worker is later removed; a set since removed from `env_sets` makes its calls fail, with a problem line
naming the set and the worker. When more than one worker is configured, a run's worker shows beside its model.

- **Each `CLAUDE_CONFIG_DIR` is a Claude Code of its own.** Log in to each once (`claude` with that variable
  set), and run `contour agent setup` in each, so its status hooks are installed:
  `$env:CLAUDE_CONFIG_DIR = 'C:\Users\me\.claude-work'; contour agent setup` on Windows,
  `CLAUDE_CONFIG_DIR=/home/u/.claude-work contour agent setup` elsewhere. The hooks are checked once per set,
  and the problem line names the worker (`Claude Code's status hooks are missing for worker claude-work: …`);
  the plugins are checked and installed per set as well.
- **A Copilot run** starts `copilot --model {model} -i {prompt}` in its worktree, with the task's model, which,
  as for every task, must be one of `models`. A Copilot task's model is a Copilot model id (`auto`, or one
  `copilot --help` lists), not a Claude Code alias such as `sonnet`, so `models` lists it too. Copilot reports no status, so the run shows *running* until its
  tab closes, and then ends `Closed`, a normal finish. It has no transcript (`t` says Copilot runs keep none
  Contour can read), no statistics and no board. It takes no permission profile: a task with `permissions` on a
  Copilot worker is a problem line, and the task is not offered. Its set may hold `COPILOT_HOME` and the proxy
  variables. On Windows it must be a real `copilot.exe`: npm's `copilot.cmd` is refused, as `claude.cmd` is.

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
