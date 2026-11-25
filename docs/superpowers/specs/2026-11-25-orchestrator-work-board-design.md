# Orchestrator Work board — design

Interactive mockup of this design: the artifact "Orchestrator Work Board" (private to its owner). The mockup
illustrates; this document is binding. Column order is as in **A row** below; widths, the details line and the
dialogs' layout follow the dashboard's existing conventions and are the builder's to fix in screen tests.

## Intent

The user starts agents on their own merge requests and Jira tickets. Today that happens in the launcher, a
search popup (`n`, `view/Launcher`, `view/DispatchForm`). The user's complaints:

- It does not show **what is happening with their merge requests** (pipeline, review, conflicts) next to **the
  Jira status of the ticket** each one belongs to.
- The list **does not scroll well and nothing folds**.
- The **important items are not separated** from the rest; there is no sorting or filtering by what needs them.
- **The worker cannot be chosen**: a run's worker is fixed by its task (`tasks[].worker`).

They also asked for **mouse support** throughout, and for `n` to **start a custom task directly** rather than
open a search.

Success: one glance at the Work tab says what needs the user; one double-click or `⏎` on a row, then `⏎`
again, starts an agent on it, on the worker the user picks.

## Goals

1. A **Work** view, a tab beside the dashboard's Agents view, listing the user's open merge requests and their
   Jira tickets, in five groups ordered by importance (below).
2. Each merge request row shows its **pipeline**, **review**, **conflicts**, the **Jira status** of its ticket, and
   the **worker** of an agent already on it.
3. Groups **fold**, and folds persist across restarts like the Fleet's folds do today (`core/EngineFolds.cpp`).
   The list **scrolls** with the selection kept in view.
4. **Filter** (`/`, matching number, key, title, repository), **repository** chip, and a **sort** switch:
   by importance (grouped, the default) or by last update (one flat list).
5. A **start dialog** over the board: task, **worker**, model, note; the task is suggested from the row's state,
   the worker defaults sensibly, and every configured worker can be picked.
6. **`n` starts a custom task**: a text box; `⏎` starts at once with defaults filled in.
7. **Mouse** for everything (table below), reusing the dashboard's hit map.
8. **Jira's grouping is configurable by query**, defaulting to Jira's status categories.

## Non-goals

- Merge requests where the user is a reviewer (possible sixth group later).
- Acting on items beyond starting agents and opening them in the browser: no merging, approving, or Jira
  transitions from the board.
- Changing how a run executes once started: dispatch, worktrees, naming and the run record stay as they are.

## Anti-goals

- **Bounded polling cost.** Per repository and poll the board adds at most: one GitLab GraphQL call (below), one
  `acli` call for tickets that MRs name but the issue search did not list, and — only when configured — one `acli`
  call for `in_progress_jql`. Never one call per MR.
- **No hidden decisions.** The suggested task and worker are shown in the dialog, with the reason, and can be
  changed before starting.
- **Keyboard parity.** Nothing is mouse-only; every mouse action has a key.

## Approaches

- **A — a Work view of its own (chosen).** A second dashboard tab: a full-width, scrolling, folding board; a small
  start dialog over it. Room for all status columns; also useful just to look.
- **B — reorganize the launcher popup.** Same groups, folding and worker choice inside today's `n` popup. Less to
  learn, but statuses shrink to symbols and the list shares its height with the settings.
- Rejected without a sketch: per-group tabs (hides the ordering, which is the point); a separate extension (would
  duplicate the engine's polling and run bookkeeping).

The user chose **A**, and decided that `n` no longer opens the search launcher but starts a custom task.

## The groups

Every item is in exactly one group, the **first** row of this table that matches. Inside a group, the most recently
updated item comes first.

| # | Group | An item is here when |
|---|---|---|
| 5 | **Everything else** | A **draft** MR — checked first, whatever its pipeline or conflicts. |
| 1 | **Needs you** | An MR whose pipeline failed, or a reviewer requested changes, or it conflicts with its base. |
| 2 | **Ready to merge** | An MR whose pipeline passed (or that has no pipeline), with no conflicts, and approved — or with no review required (GitHub `reviewDecision` empty; GitLab: no approval rule left unsatisfied). |
| 3 | **In review** | Any other open MR (pipeline running, review pending). |
| 4 | **No MR yet** | A ticket or forge issue that **is in progress** (below) and that no MR names. |
| 5 | **Everything else** | Tickets and issues that are not in progress. Folded by default. |

The groups are shown in the order 1–5; the table's row order is only the matching order. A ticket or issue an MR
names is not listed on its own; its status shows on the MR's row.

**Forge issues** (`source: github` / `gitlab`) take part like Jira tickets: an MR names one through the forge's
own link (GitHub `closingIssuesReferences`, today's `closesIssues`) or a `#<number>` in its title; an open forge
issue listed by the source counts as **in progress** (it has no status category; the source's query already
selects the user's work). Its status column shows `● open`. The launcher's ability to start an agent on any listed
issue is kept this way.

**Which repository an item belongs to.** Items are de-duplicated by key (Jira key, or repository + number for forge
items). A ticket listed by several repositories' sources belongs to the **first repository in configuration
order** whose source lists it; that repository is where the start dialog checks it out. An MR may link a Jira key
from **any** configured Jira source's projects, not only its own repository's.

**Linking an MR to its ticket.** The first Jira key (`[A-Z][A-Z0-9]+-[0-9]+`) in the MR's title, else in its source
branch name. The key must belong to a project some configured Jira source covers (the project part of a key that
those sources listed, or a project named in their JQL) — the builder decides the exact rule; the goal is to avoid
matching a stray token such as `UTF-8`. An MR with no key shows `·` in the Jira column.

**"In progress", by default.** Jira reports a status category with every status: `statusCategory.key` is `new`,
`indeterminate` or `done`, whatever the project calls the status (verified against `acli jira workitem search
--json`; the default `--fields` includes `status` with its `statusCategory`). A ticket is in progress when its
category is `indeterminate`. This needs no configuration and no extra request.

**"In progress", by query (configurable).** A repository's Jira issue source may carry an optional
`in_progress_jql`. When set, a ticket is in progress when it matches that JQL: the orchestrator runs
`acli jira workitem search --jql "(<source jql>) AND (<in_progress_jql>)" --fields key --json` once per poll and
uses the keys it returns. Example: `in_progress_jql: 'status in ("In Progress", "Selected for Development")'`.
The text is an `ArgumentKind::Text` argument like the source's own `jql`. A failed query leaves the default rule in
force and shows a problem line, as other failed polls do.

**Tickets that MRs name but the issue search did not list** (assigned to someone else, or outside the JQL) get
their status from one `acli jira workitem search --jql "key in (<keys>)" --json` per poll, so every linked MR shows
a Jira status. Keys are validated against the key pattern before they reach the command.

## A row

| Column | MR row | Ticket row |
|---|---|---|
| MR | `!812` / `#2375` | — |
| Ticket | its linked key | its key |
| Title | sanitised title, cut with `…` | same |
| Pipeline | `✗ pipeline` / `◌ running` / `✓ pipeline` / `·` | `·` |
| Review | `⚠ conflicts` (wins) / `⟲ changes` / `✓ approved` / `… review` / `· no review` | `·` |
| Jira | `◆ <status name>` | same |
| Agent | `⬢ <worker>` when a run works on the item, else the age of its last update | same |

Glyph and colour come in pairs, as on the dashboard today. A details line under the list names the failing checks,
the conflict's base branch, the linked ticket and its status, and the agent on it.

**GitHub** already supplies what the row needs in one `gh pr list` call: checks with names, `reviewDecision`, and
`mergeable` (`CONFLICTING`) — added to the requested JSON fields if missing.

**GitLab** does not: `glab mr list --output json` (the REST list) has no pipeline object and no approvals, and
today's mapping infers CI and review from `detailed_merge_status`, which holds one value at a time (it never says
"pipeline passed", names no failing job, and reports a failed pipeline only where `ci_must_pass` is set — so a
failing MR could land in Ready to merge). The board therefore adds **one GraphQL call per GitLab repository per
poll** (`glab api graphql`, a new declared command) that returns, for the user's open MRs of that project:
`headPipeline { status, jobs(statuses: FAILED) { name } }`, `approved`, `approvalsLeft`, `conflicts`,
`draft`, `updatedAt`. The REST list stays the source of which MRs exist; the GraphQL result enriches them by IID.
If the GraphQL call fails, rows fall back to today's `detailed_merge_status` inference and a problem line says so.

**Updated** comes from the MR's `updated_at` / `updatedAt`, a Jira ticket's `updated`. `updated` is not among
`acli`'s default fields (`core/testdata/jira/README.md`), so the declared `acli-issues` command gains
`--fields key,summary,status,issuetype,updated`. Existing users see that changed command once in the consent
dialog; that is accepted.

## The start dialog

Opened by `⏎` or a double-click on a row, or "Start agent…" in its right-click menu.

- **Task** — suggested from the row, first match wins: pipeline failed → *fix CI*; conflicts → *resolve
  conflicts*; changes requested → *address review*; a ticket → *work on issue*; otherwise the first configured task
  that applies to the item. "Suggested" means the configured task whose built-in role is that one; the builder maps
  roles to the user's `tasks` (today a task's `applies_to` and prompt; a small explicit `role:` per task may be
  needed — builder's call, keeping today's configurations valid). The reason is shown beside it
  (`pipeline fails: test-windows`).
- **Worker** — every configured worker, each with how many runs it has now. Default: the task's own worker; when
  that worker already runs at least one agent, the worker with the fewest runs (ties: configuration order). A worker
  whose setup is incomplete (hooks or plugins missing; today's `isSetupIncomplete`) is marked, and starting on it
  asks for a second `⏎`, as the launcher does now.
- **Model** — the task's model, changeable as today.
- **Note** — optional text added to the prompt, as today.
- Below: checkout and worktree, derived as today. `Ctrl+P` shows the prompt and command line, as today.
- When an agent already works on the item, the dialog says so and asks for a second `⏎`, as today's engine refusal
  does.

The chosen worker overrides the task's worker for this run only. The run record already stores its worker
(implicit worker: no key), so nothing changes in the stored format.

## `n`: a custom task

`n` (and `+ new task` in the key bar) opens a box with the cursor in "What should the agent do?". `⏎` starts at
once; nothing else must be chosen. Defaults: **repository** — the repo chip when it names one, else the selected
row's repository, else the first configured one; **worker** — the topic task's worker, with the same busy rule as
above; **model** — the topic task's. `Tab` or a click reaches those three settings. The text becomes the prompt
through the configured topic task, or the built-in one; naming, branch and worktree follow today's topic runs.
An empty text does not start.

**The search launcher is removed.** Its other jobs move: picking a PR/issue → the Work view and its `/` filter;
a query naming an item exactly (`#2375`, a URL, a Jira key) → the filter matches it; reopening a finished topic
run's text (`⏎` on it today) → opens the `n` box with that text. Its scope chips and `Ctrl+←→` kind switching go
away. Screens in `view/testdata/launcher-*` are replaced by Work view screens.

## Mouse

The dashboard already enables SGR mouse reporting and records each frame's hit regions (`view/HitMap.hpp`). The
Work view adds regions for what it draws.

| Where | Click | Double-click | Right-click | Wheel |
|---|---|---|---|---|
| Row | select | open the start dialog | menu: start agent, open MR, open ticket, copy branch name, fold the group | scroll the list 3 lines; selection stays until it leaves view |
| MR number / ticket key | select the row | open it in the browser | as on the row | as on the row |
| Group heading | fold / unfold | same | fold all other groups | scroll |
| Scrollbar (right edge of the list) | jump there | — | — | drag the thumb |
| Filter, repo chip, sort chip | focus the filter; step the chip | — | step back | step |
| Key hint in the bar | run that action | — | — | — |
| Start dialog / `n` box | pick a setting; on its value or `‹ ›`, step it; a worker chip picks it | start | step back | step the setting under the pointer |
| Outside a dialog or menu | close it | — | — | — |

**Hover highlight** needs any-motion reporting (DECSET 1003). It is optional: the board redraws only when the row
under the pointer changes; if the host or terminal does not deliver motion, everything else works unchanged. The
builder checks whether the extension host passes motion events and whether a drag (button-held motion, 1002) is
available for the scrollbar; without it, the scrollbar takes clicks only.

## Keys

| Key | Work view |
|---|---|
| `↑` `↓` `PgUp` `PgDn` `Home` `End` | move the selection |
| `⏎` | start dialog on a row; fold/unfold on a heading |
| `z` | fold / unfold the selected row's group |
| `/` | filter; `Esc` clears it |
| `s` | sort: importance ⇄ last update |
| `←` `→` | step the repository chip |
| `o` | open the selected MR or ticket in the browser |
| `n` | custom task |
| `w` | switch between the Agents view and the Work view (also a click on the view's name in the top bar) |
| `Ctrl+K` | the palette, as everywhere |

**Where the Work view sits.** The dashboard's top bar names two views, `Agents` and `Work`. The Agents view is
today's dashboard, Fleet and Focus, unchanged, with `Tab` still moving between them. The Work view replaces both
panes while it is shown; the top bar and the Fleet pill stay. `w` is not reserved today (`core/DashboardKeys.cpp`);
`s` and `o` are not either. In the Work view `Tab` moves between the list and the filter bar's chips.

## Configuration

Additions only; every configuration valid today stays valid.

```yaml
repos:
  - path: …
    issues:
      - source: jira
        site: https://example.atlassian.net
        jql: 'assignee = currentUser() AND resolution = Unresolved'
        in_progress_jql: 'status in ("In Progress", "Selected for Development")'   # optional
```

Whether the start dialog's task roles need a per-task `role:` is the builder's call (above). Nothing else is
configured: groups, columns and ordering are fixed by this design. Changed declared commands (consent shows them
once): `acli-issues` gains `--fields`; a new `glab api graphql` command; a new `acli` command for `key in (…)`
lookups and the optional `in_progress_jql`.

## Calls made while writing this (not discussed with the user)

- GitLab rows are enriched by one GraphQL call per repository per poll, rather than shown with less information.
- Drafts always go to Everything else; "no review required" counts as approved.
- Forge issues take part as always-in-progress items, so nothing the launcher could start is lost.
- A ticket listed by several repositories belongs to the first in configuration order.
- `w` switches views; `Tab` keeps its Fleet/Focus meaning.

## Constraints

- The orchestrator stays free of Contour's `src/` (it may move to its own repository).
- Every new or changed command goes through the manifest's declared commands and their argument kinds; the
  consent dialog must still show exactly what runs.
- Rendering stays within the daemon's call deadline (`src/vtextension/DaemonLimits.hpp`); a long list renders
  only its visible lines.
- Folds and the chosen sort persist with the existing fold state; the filter does not persist.
- Screen tests (`view/testdata/*.screen`) at the usual sizes (50×20, 80×24, 120×40, 160×40) for: the grouped
  board, a folded group, the flat sort, a filter, the start dialog, the `n` box, the right-click menu.
- Docs: the orchestrator README's launcher section is rewritten for the Work view and `n`; `docs/extensions.md`
  gains `in_progress_jql`.

## Left to the builder

- The exact MR→ticket key-matching rule (project restriction).
- Task roles for suggestion: inferred or an explicit `role:`.
- Whether drag and hover are available from the host; degrade as described.
- Narrow widths: which columns drop first (suggested: Agent age, then Review text → glyph, then Pipeline text → glyph).
- Module layout: likely `view/WorkBoard.*`, `view/StartDialog.*`, `view/TaskBox.*`, a `core/WorkItems.*` that joins
  MRs, tickets and runs into groups, and the removal of `view/Launcher.*` with the parts of `view/DispatchForm.*`
  the dialog still needs.
