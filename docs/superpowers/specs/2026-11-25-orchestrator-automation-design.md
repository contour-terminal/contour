# Orchestrator automation: rules that start agents — design

Follows the Work board (2026-11-25-orchestrator-work-board-design.md) and the auto-mode / vim-keys batch
(2026-11-25-orchestrator-defaults-and-vim-keys-design.md).

## Intent

The user wants agents started for them instead of picking each one by hand: "go over all open MRs and fix CI
there", "when I get a ticket without an MR, auto-trigger working on it". Agents started this way are the same
agents as today — they run in their own tabs, the user watches them on the dashboard and can chat with them;
only the decision to start one is automated.

This reverses an earlier non-goal of the orchestrator ("automatic dispatch"). The user decided it; the safety
rails below are what make it acceptable.

## Goals

1. **Rules** in the orchestrator's configuration: each rule names a condition the Work view already detects
   and a task to start on every matching item, optionally with its own worker, model and permission profile.
2. **Bulk, on demand**: a palette command "run a task on this group" starts a task on every item of the
   selected Work group, once.
3. **Safety rails**: one agent per item; one attempt per item state; a cap on automatic agents with a queue;
   one switch that pauses all rules; automatic agents visibly marked.
4. Everything configurable; every configuration valid today stays valid and starts nothing automatically.

## Non-goals

- New conditions the Work view does not already compute (e.g. "MR older than N days", review comments).
- Scheduling by time of day, cron syntax, or chaining tasks (start B when A finishes).
- Acting on items the Work view does not list (other people's MRs, unassigned tickets).
- Changing how an agent runs once started.

## Anti-goals

- **A runaway**: a rule that keeps restarting agents on an item that keeps failing, or floods every worker.
- **Silent automation**: an agent started by a rule that the user cannot tell apart from one they started, or
  rules active without the user having turned them on.
- **Two agents on one item**, from a rule and the user, or from two rules.

## Configuration

Under the orchestrator's `config:`; all optional.

```yaml
automation:
  enabled: false           # the master switch's stored default; the dashboard switch overrides it (below)
  max_agents: 3            # automatic agents running at once; default: number of workers − 1, at least 1
  rules:
    - { when: pipeline_failed, task: fix-ci,        worker: claude-n8n,   model: sonnet }
    - { when: no_mr_yet,       task: work-on-issue, worker: claude-james, model: opus, permissions: auto }
```

- `when` — one condition, from a fixed table the Work view already computes: `pipeline_failed`,
  `conflicts`, `changes_requested`, `ready_to_merge`, `no_mr_yet` (a ticket or forge issue in progress with
  no MR). Each maps to the Work view's per-item state (core/WorkItems), not to the group name, so a rule's
  meaning does not change if grouping changes. An unknown name is a configuration problem naming the known ones.
- `task` — a configured task id (or the built-in topic task is not allowed: rules act on items). The task
  must apply to the matched item kind (`applies_to`); a mismatch is a configuration problem.
- `worker`, `model`, `permissions` — each optional; omitted ones come from the task, exactly as the start
  dialog's defaults do (worker: the task's worker, or the least busy one when that one is busy; model and
  permission profile: the task's). A worker the engine would refuse for the task (e.g. one that takes no
  profile) is a configuration problem.
- Two rules may match one item; the first rule in configuration order wins for that item.

## Behaviour

**When rules run.** After each poll, when the work items are rebuilt, the engine evaluates every rule
against every listed item, in Work-view order (most important first), and starts matching items until the cap
is reached. Nothing is evaluated while automation is paused.

**One agent per item.** An item that already has a run (any state but finished) is skipped, whoever started it.

**One attempt per state.** For each rule and item the engine remembers the item's state signature when it
last started an automatic agent: for an MR, its head commit and pipeline id; for a ticket, its key and status.
After that run finishes (whatever its outcome) the rule does not fire again on that item until the signature
changes (a new commit, a new pipeline, a status change). The memory is stored so it survives restarts, and is
dropped for items that leave the list.

**Cap and queue.** At most `max_agents` automatically started runs are active at once (runs the user started
do not count, and are never blocked by automation). Matches beyond the cap wait in a queue in Work-view order;
the queue is re-evaluated at every poll, so an item that stops matching leaves it.

**The switch.** One switch pauses or resumes all rules: shown in the Work view's top bar (`auto ▶ on` / `auto ⏸
paused`), toggled by a key (proposed `A`, checked against the dashboard's reserved keys and `keys` clashes) and a
palette entry, and by a click. Its state is stored. **After an update that introduces automation, or when
`automation` is first configured, the switch starts paused**; the user turns it on once. `enabled:` in the
configuration is the default only when nothing is stored.

**Marking.** A run started by a rule records its origin (rule index and `when`) in the run record; the Agents
list shows a mark (e.g. `⚙`) beside it, its details say which rule started it, and the Work view shows
`⚙ queued` on items waiting in the automation queue.

**Bulk: "run a task on this group".** A palette entry, available in the Work view on a group heading or any row:
it opens a small dialog naming the group, the task (suggested per item as today, or one task for all), the
worker and model (as in the start dialog), and the count, e.g. "Start fix CI on 4 merge requests". `⏎`
confirms. It obeys "one agent per item", uses the same queue, and counts against `max_agents`; it does not
create a standing rule and does not use "one attempt per state".

**When an automatic run is done.** A run started by a rule (or by bulk) that reaches Done ends itself: the
engine ends it the way `x` does, so its worktree is removed. The usual safety still applies: a worktree with
uncommitted or unpushed work is kept, the run is listed as kept, and the user is told why. A run the user started
is never ended automatically. A run that fails or stops asking for input is not ended; it waits for the user, and
it still counts against `max_agents` until it is ended.

**Without a window.** Rules run in the daemon and keep firing when no Contour window is attached: polls continue,
matching items start agents up to the cap, and the dashboard shows what happened the next time it opens. The
switch is the only way to stop automation, and its stored state applies whether or not a window is open.

**Failure handling.** A rule start that the engine refuses (e.g. a worker's setup is incomplete) is not retried
until the next poll and shows as a problem line naming the rule; three consecutive refusals pause that rule and
say so.

## Approaches

- **Inside the orchestrator's poll (chosen).** The engine already fetches MRs and tickets, computes their
  states and knows the runs and workers; a rule is "after the poll, dispatch as the start dialog would".
- A separate extension or a scheduled job: would duplicate fetching, grouping and run bookkeeping.
- Claude-side automation: cannot see MRs, tickets and workers together.

## Constraints

- The orchestrator stays free of Contour's `src/`; no new host capability.
- Data-driven: the conditions are a table; adding one is a row.
- The daemon's call deadline: rule evaluation is bounded by the item count and runs after the poll, not per
  frame.
- Docs: README and docs/extensions.md (via the controller's WIP-preserving procedure) describe `automation`.

## Decided by the user

- An automatic run that reaches Done ends itself and its worktree is removed (kept if it holds unsaved work).
- Rules keep starting agents while no Contour window is attached to the daemon.

## Decided by the controller (not discussed in detail with the user)

- Rules start paused after an update; one switch for all rules.
- Bulk asks for one `⏎` with a count.
- `max_agents` default: workers − 1 (at least 1).
- First matching rule wins per item.
- Three consecutive refusals pause a rule.

## Left to the builder

- The exact state signature per item kind, if a better one exists in the data.
- The switch's key (after the clash check) and its exact text.
- Where the automation memory and switch are stored (the existing store).
