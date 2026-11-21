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
  into a pane, reading a screen, or choosing Claude's settings; untrusted PR or issue text reaching a
  prompt, a branch name or a path.

## The dashboard

The orchestrator draws its own tab (`view/`, drawn with core-cpp's headless terminal UI): your open pull
requests with their checks and review, the issues no pull request of yours closes, your agents with their
state and age, and the Finished list, each entry marked `●` until you open it. While a worktree is left
that no run accounts for — on an `agent/…` branch or under `worktree_root` — a LEFTOVER WORKTREES section lists
it with its branch, or `detached`: a run that ended without removing its worktree leaves one, as does a worktree
made by hand under the root. One a run has just removed is not listed while the last listing predates the
removal; the repository's worktrees are listed again at once. The orchestrator never removes one: Enter opens
its details on the whole
width, with the `git -C <checkout> worktree remove <path>` line to run by hand from anywhere, each path
quoted for the shell (a path that would not show as it is, such as one holding a zero-width character, gets
no line), and git refuses it while it holds changes.
Problems — a failing `gh`,
a list cut at its limit, a configuration that does not parse — sit above the lists, one line each, and
the line at the bottom is the summary the status line shows.

The layout follows the tab's size:

| Width | Layout |
| --- | --- |
| 100 columns or more | the list and the selected item's details side by side |
| 60 to 99 | the list, the details below it; Tab swaps the details in for the list |
| below 60 | the list alone; Tab swaps the details in for the list, Esc returns |

Below 20 rows the key-hint line and the section headings go first. A list row is cut with `…` where it is
too wide, never in the middle of a wide character. The details wrap by default, and so does the dispatch
form; a command line or a path among the details is cut instead, never wrapped, so that what is shown is
never broken where a space is. Every title is sanitised before it is drawn, so an escape sequence in a pull
request's title is shown as text and does nothing.

| Key | Does |
| --- | --- |
| `↑` `↓` `j` `k` `Home` `End` | move the selection |
| `Enter` | on a pull request or an issue, open the dispatch form on the first task that applies to it, or show its agent's tab if one runs; on an agent, show its tab; on a finished run, open its details (and mark it seen); on a leftover worktree, open its details on the whole width |
| `d` | open the dispatch form on the selected item; on a finished run, on its item, for a new run |
| `b` | show the selected run's board (below) |
| `t` | show the selected run's transcript (below) |
| a task's `key` | open the dispatch form on that task |
| `Esc` | close the help, the details page or the filter |
| `Tab` | swap the details in for the list, and back (below 100 columns) |
| `/` | filter the rows by text; Enter keeps the filter, Esc drops it |
| `?` | show the keys |
| `R` | read the lists again now |
| `e` | end the selected agent's run |
| `x` | check a kept worktree again |
| `Del` | drop a finished run from the list |

The dispatch form shows the item, the task and the model (`←` `→` change the one selected, `Tab` moves
between them and the note), the prompt, the checkout, the worktree and the agent's exact command line,
then how many agents run. `Enter` dispatches; `Esc` cancels. A task's key may not be one of the
dashboard's own: the configuration is refused with a line that names them. While Claude Code's status
hooks are missing the form says so, and asks for a second `Enter`.

## Boards and transcripts

Both are screens of the dashboard's tab, not tabs of their own, and both are read-only.

**The board** is the agent's `STATUS.md` at its worktree's root, read with `file-read` from the `worktrees`
root and shown with how long ago it was written, again every five seconds while it is shown. The extension
does not ask an agent to write one (a hook's request proved unreliable); a task's prompt does. Once an agent
has run for `status_board_after` the engine looks for the file, and the agent's row carries `▤` when one is
there. A board is untrusted text: drawn inert, cut at 64 KiB, and refused when it leads outside the worktree.
Ending a run removes an untracked `STATUS.md` (`git clean -f -- STATUS.md`) before the worktree is checked for
work, so a board never keeps a worktree. `b` is offered on a finished run only while its worktree was kept,
and such a board is read once, not every five seconds; a board shown when its worktree goes says that it was
removed when the run ended, never that the prompt failed to ask for one.

**The transcript** is the run's Claude Code session log, a page of 30 entries at a time, read with the
declared command `agent-transcript` (`contour agent transcript … --format 1 --json`). The host finds the log
by the run's session id and binds it to the run's worktree, which need not exist any more, so a finished run
keeps its transcript. The page's JSON is a contract (`contract/agent-transcript/`): a page in another format,
or Contour's refusal of the one asked for, shows "format mismatch: update contour or the orchestrator", never
a parse error. A read that fails shows the command to run by hand.

| Key | Does, on a board | Does, on a transcript |
| --- | --- | --- |
| `↑` `↓` `j` `k` `PgUp` `PgDn` | scroll | scroll |
| `Home` `End` | the top and the bottom | the first and the last page |
| `←` `→` | | the previous and the next page |
| `b` | read it again now | show the board |
| `t` | show the transcript | |
| `d` | the dispatch form on the run's item, for a new run | the same |
| `Enter` | show the agent's tab | the same |
| `Esc` | back to where it was opened from: the list or the transcript | back to where it was opened from: the list or the board |

Its screens are pinned by golden files in `view/testdata/`, one per scene and size; after a deliberate
change, `ORCHESTRATOR_UPDATE_GOLDENS=1 orchestrator_test "[view]"` writes them again, to be checked by eye.

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
