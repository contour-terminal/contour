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

- **Success:** four agents dispatched in a minute; their state visible at a glance from any window
  attached to the daemon, or from none; worktrees gone when their runs end, unless they hold work.
- **Non-goals:** automatic dispatch; deciding what an agent may do. Claude Code's permission mode,
  the agent's skills and memory, and the forge's branch protection decide that — including whether an
  agent may push to a protected branch.
- **Anti-goals (a failure even if it works):** an agent's work lost by cleanup; the extension typing
  into a pane, reading a screen, or choosing Claude's settings; untrusted PR or issue text reaching a
  prompt, a branch name or a path.

## The dashboard

The orchestrator draws its own tab (`view/`, drawn with core-cpp's headless terminal UI): your open pull
requests with their checks and review, the issues no pull request of yours closes, your agents with their
state and age, and the Finished list, each entry marked `●` until you open it. Problems — a failing `gh`,
a list cut at its limit, a configuration that does not parse — sit above the lists, one line each, and
the line at the bottom is the summary the status line shows.

The layout follows the tab's size:

| Width | Layout |
| --- | --- |
| 100 columns or more | the list and the selected item's details side by side |
| 60 to 99 | the list, the details below it; Tab swaps the details in for the list |
| below 60 | the list alone; Enter opens the details as a page, Esc returns |

Below 20 rows the key-hint line and the section headings go first. A list row is cut with `…` where it is
too wide, never in the middle of a wide character. The details wrap by default, and so does the dispatch
form; a command line or a path among the details is cut instead, never wrapped, so that what is shown is
never broken where a space is. Every title is sanitised before it is drawn, so an escape sequence in a pull
request's title is shown as text and does nothing.

| Key | Does |
| --- | --- |
| `↑` `↓` `j` `k` `Home` `End` | move the selection |
| `Enter` | open the details (and mark a finished run as seen) |
| `Esc` | close the help, the details page or the filter |
| `Tab` | swap the details in for the list, and back (60 to 99 columns) |
| `/` | filter the rows by text; Enter keeps the filter, Esc drops it |
| `?` | show the keys |
| `R` | read the lists again now |
| `e` | end the selected agent's run |
| `x` | check a kept worktree again |
| `Del` | drop a finished run from the list |

Its screens are pinned by golden files in `view/testdata/`, one per scene and size; after a deliberate
change, `ORCHESTRATOR_UPDATE_GOLDENS=1 orchestrator_test "[view]"` writes them again, to be checked by eye.

## Building

The project builds inside Contour, whenever `CONTOUR_EXTENSIONS` is on, and takes every dependency from
that tree: nothing is fetched. Its tests build with `ORCHESTRATOR_TESTING`, which follows `CONTOUR_TESTING`:

    cmake --build --preset clang-asan --target orchestrator_test
    out/clang-asan/extensions/orchestrator/orchestrator_test

It includes nothing from Contour's `src/`, so that it can move into a repository of its own. Sources
include each other as `<orchestrator/core/X.hpp>`, so a standalone checkout must be a directory named
`orchestrator`. Configured on its own, it fetches glaze, libunicode and core-cpp at the versions Contour
carries, and Catch2 too with `-DORCHESTRATOR_TESTING=ON`.
