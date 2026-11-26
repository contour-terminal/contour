# Orchestrator automation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rules in the orchestrator's configuration start agents on Work-view items by themselves, and a palette
command starts one task on every item of a Work group, behind a cap, a queue, one pause switch and visible marks.

**Architecture:** A pure planner (`core/Automation.*`) decides, from the work items in Work-view order, the rules,
the stored attempt memory and the free slots, which items start now and which wait. The engine runs it at the end
of every step and asks the driver to start each chosen item with a new effect, `StartAutomatic`. The driver mints
the agent's session id and steps the `Dispatch` back in, exactly as the start dialog's dispatch. The run records
its origin, which makes it end itself at Done and shows `⚙` on the dashboard.

**Tech Stack:** C++23, Catch2, glaze (JSON), the orchestrator's core/view/guest layers (WASM guest).

**Spec:** `docs/superpowers/specs/2026-11-25-orchestrator-automation-design.md`

## Global Constraints

- Every configuration valid today stays valid and starts nothing automatically: no `automation` key means no rule.
- The orchestrator stays free of Contour's `src/`; no new host capability; no new declared command (the manifest
  and its consent fixture stay unchanged).
- Data-driven: conditions are one table (`AutomationConditionRows`); adding one is a row.
- Project rules (AGENT.md): `enum class` over `bool` (zero = off/absent), `[[nodiscard]]`, Doxygen on every new
  public name, range-based loops only, zero warnings, clang-format.
- No real company names, people, hosts or local paths in code, tests, fixtures, docs or commit messages.
- Never touch, stage, revert or stash `docs/extensions.md` or `cmake/ContourWasiCxx.cmake` through the working
  tree; Task 9 changes `docs/extensions.md` through the index only.
- Build: `cmake --build --preset clang-debug --target orchestrator_test --parallel 2` (one build at a time).
  Test: `out/clang-debug/bin/orchestrator_test.exe "<name>"`, then
  `ctest --preset clang-debug -R "orchestrator|check_include_budget"`.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never push.

## Decisions this plan takes (the spec left them to the builder)

- **State signature:** an MR row: `sha:<headSha>`, or `upd:<updatedWallMs>` when the forge gave no head commit;
  a ticket row: `status:<status>`, or `cat:<StatusCategory as number>` when the status is empty. A pipeline re-run
  on the same commit does not fire a rule again; that guards against runaways.
- **Rule identity** in the memory is `<when>/<task>`, not the rule's index, so reordering rules keeps the memory.
- **Drafts never match** an MR condition: they are the user's own unfinished work.
- **The switch has no key.** It is a chip in the Work view's filter bar (⏎ or click) and a palette entry. Binding
  `A` would refuse configurations whose task already uses `A`, breaking "valid today stays valid".
- **The store:** one key, `automation/state`, holding the switch, the memory and the paused rules. Nothing stored
  means the switch is `enabled:`, `false` by default, so the switch starts paused.
- **Bulk** runs whether or not the switch is on: it is the user's explicit act. Its dialog chooses the task
  (suggested per item, or one task for all). The worker and model are each task's defaults, shown but not chosen.
- **A paused rule** (three refusals in a row) resumes when the switch is turned off and on again.
- **Capacity refusals** (`AgentLimit`, `RepoLimit`, `AlreadyRunning`) are not counted: the item stays queued.
- **A worker whose hooks are `Missing`** is a refusal for a rule ("its worker's hooks are missing"); the start
  dialog only warns, but nobody watches an automatic start.
- **The planner runs at every step's end**, not only after a poll: it works from the last poll's items, so a
  finished automatic run's slot is refilled at once, and the memory stops a refire on an unchanged item. Pruning
  the memory of items no longer listed happens only when a poll completes.

## Review Focus

1. **The daemon with no dashboard tab:** a `StartAutomatic` with zero served tabs must still be stepped as a
   `Dispatch` (Task 6 test `automatic start runs with no tab served`).
2. **A restart while a `StartAutomatic` is in flight:** `_automaticPending` is not stored; after a restart the item
   is evaluated again, and its active run (if the Dispatch got through) makes it skip. No double start (Task 4 test
   `pending start is not issued twice`).
3. **A rule whose item keeps failing:** after its automatic run finishes, an unchanged signature never refires,
   even across a restart (Task 4 test `memory survives a restart`).
4. **A Done automatic run with unpushed work:** it ends, and the removal keeps the worktree as today (Task 5 test
   `automatic run with dirty worktree ends kept`).
5. **The configuration removes a rule** while its items are queued: the queue empties on the next step, and the
   rule's memory is kept until its items leave the list (Task 4 test `removed rule leaves the queue`).

---

### Task 1: Conditions, rules and the configuration's `automation`

**Files:**
- Create: `extensions/orchestrator/core/AutomationRules.hpp` (no `.cpp`: constexpr table and plain structs)
- Modify: `extensions/orchestrator/core/Config.hpp`, `core/Config.cpp` (parse `automation`, new problems,
  `describe()` rows)
- Test: `extensions/orchestrator/core/Config_test.cpp`

**Interfaces:**
- Produces (`AutomationRules.hpp`, namespace `orchestrator`):
  ```cpp
  enum class AutomationCondition : uint8_t { PipelineFailed = 0, Conflicts, ChangesRequested, ReadyToMerge, NoMrYet };
  struct AutomationConditionRow { AutomationCondition condition; std::string_view name; AppliesTo appliesTo; };
  inline constexpr auto AutomationConditionRows = std::array { /* pipeline_failed, conflicts, changes_requested,
      ready_to_merge: PullRequest; no_mr_yet: Issue */ };
  [[nodiscard]] constexpr std::optional<AutomationCondition> automationConditionNamed(std::string_view) noexcept;
  [[nodiscard]] constexpr std::string_view automationConditionName(AutomationCondition) noexcept;
  enum class AutomationSwitch : uint8_t { Paused = 0, On };
  struct AutomationRule {
      AutomationCondition when; std::string task; std::string worker = {}; std::string model = {};
      std::string permissions = {}; bool operator==(AutomationRule const&) const = default; };
  /// `<when>/<task>`: the rule's identity in the memory and the paused list.
  [[nodiscard]] std::string ruleKeyOf(AutomationRule const& rule);
  struct AutomationSettings {
      AutomationSwitch enabled = AutomationSwitch::Paused; std::optional<uint32_t> maxAgents = std::nullopt;
      std::vector<AutomationRule> rules = {}; bool operator==(AutomationSettings const&) const = default; };
  /// @return `maxAgents`, else workers − 1, at least 1.
  [[nodiscard]] uint32_t automationCapOf(AutomationSettings const& settings, std::size_t workers) noexcept;
  ```
  `AppliesTo` moves from `Config.hpp` into `AutomationRules.hpp` only if the include order needs it; otherwise
  `AutomationRules.hpp` includes nothing from `Config.hpp` and `Config.hpp` includes it.
- Produces (`Config`): `[[nodiscard]] AutomationSettings const& automation() const noexcept;`
- Produces (`ConfigProblem`, appended at the end): `UnknownCondition`, `UnknownRuleTask`, `RuleTaskMismatch`,
  `UnknownRuleWorker`, `RuleWorkerTakesNoProfile`.

- [ ] **Step 1: Write the failing tests** in `Config_test.cpp`, using the file's existing parse helpers:
  - `automation absent`: `config.automation() == AutomationSettings {}`.
  - `automation parses rules`: `{"enabled":true,"max_agents":2,"rules":[{"when":"pipeline_failed","task":"fix-ci",
    "worker":"w2","model":"sonnet"},{"when":"no_mr_yet","task":"work-on-issue","permissions":"auto"}]}` →
    `enabled == On`, `maxAgents == 2`, two rules with exactly those fields.
  - Refusals, each with its key: `when:"nightly"` → `UnknownCondition` at `automation.rules[0].when`, subject
    `nightly`; a task id not configured, or `topic`/`builtin_topic` → `UnknownRuleTask` at `.task`; `pipeline_failed`
    with an issue task → `RuleTaskMismatch` at `.task`; an unknown worker → `UnknownRuleWorker` at `.worker`; a
    task with its own `permissions` on a Copilot worker → `RuleWorkerTakesNoProfile` at `.worker`; a model not in
    `models` → `UnlistedModel` at `.model`; an unknown profile → `UnknownProfile` at `.permissions`;
    `max_agents: 0` → `Zero` at `automation.max_agents`; an unknown key in a rule → `UnknownKey`.
  - `automation cap`: `automationCapOf({}, 1) == 1`, `({}, 4) == 3`, `({.maxAgents = 7}, 2) == 7`.
  - `every condition row is named once`: the table's names are unique and `automationConditionNamed(name(c)) == c`
    for every row.
- [ ] **Step 2: Run** `orchestrator_test "[config]"`. Expected: FAIL to compile (unknown names).
- [ ] **Step 3: Implement** the header, `Config::automation()`, the `Values::automation` field, the parse of
  `automation` (after `tasks` and `workers`, which the rules are checked against), and one `describe()` row per new
  problem. Wording: UnknownCondition "is no condition; the conditions are <names, comma-joined>";
  UnknownRuleTask "is no configured task a rule can start"; RuleTaskMismatch "does not apply to the items this
  condition matches"; UnknownRuleWorker "names no worker"; RuleWorkerTakesNoProfile "takes no permission profile,
  and the task has one".
- [ ] **Step 4: Run** `orchestrator_test "[config]"`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(orchestrator): read automation rules from the configuration`.

### Task 2: A run's origin

**Files:**
- Modify: `core/Run.hpp`, `core/RunJson.hpp`, `core/Run.cpp` (to/from JSON), `core/Engine.hpp` (`Dispatch`),
  `core/EngineDispatch.cpp` (`preview()` copies the origin into the run)
- Test: `core/Run_test.cpp`, `core/Engine_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  enum class RunOrigin : uint8_t { User = 0, Rule, Bulk };   // Run.hpp
  // Run: RunOrigin origin = RunOrigin::User; std::string originRule {}; ///< ruleKeyOf() for Rule; else empty.
  // Dispatch: RunOrigin origin = RunOrigin::User; std::string originRule = {};
  ```
  JSON: `std::optional<std::string> origin` (`"rule"` / `"bulk"`, absent for User) and
  `std::optional<std::string> originRule`, written only when not User.

- [ ] **Step 1: Write the failing tests:** `run origin round-trips` (Rule with `pipeline_failed/fix-ci`, and Bulk)
  in `Run_test.cpp`; `a stored run without origin reads as the user's` (existing fixture JSON → `User`); an
  unknown origin word refuses the stored run as unreadable. In `Engine_test.cpp`: `dispatch carries its origin`:
  a `Dispatch` with `origin = Rule, originRule = "pipeline_failed/fix-ci"` → the stored run has both.
- [ ] **Step 2: Run** `orchestrator_test "run origin*"`. Expected: FAIL to compile.
- [ ] **Step 3: Implement** the fields, the JSON mapping (a table of the two spelled origins), and the copy in
  `preview()`.
- [ ] **Step 4: Run** the new tests and `orchestrator_test`. Expected: PASS, nothing else changed.
- [ ] **Step 5: Commit** `feat(orchestrator): a run records whether a rule or a bulk start began it`.

### Task 3: The planner

**Files:**
- Create: `core/Automation.hpp`, `core/Automation.cpp`, `core/Automation_test.cpp`
- Modify: `extensions/orchestrator/CMakeLists.txt` (the source after `core/WorkItems.cpp`, the test after
  `core/ArgumentKinds_test.cpp` in alphabetical order)

**Interfaces:**
- Consumes: `AutomationRule`, `AutomationCondition` (Task 1); `WorkItem`, `workItemKey()`, `suggestTask()`.
- Produces (namespace `orchestrator`):
  ```cpp
  [[nodiscard]] bool matchesCondition(WorkItem const& item, AutomationCondition condition) noexcept;
  [[nodiscard]] std::string stateSignatureOf(WorkItem const& item);
  enum class RuleState : uint8_t { Active = 0, Paused };
  /// By `<ruleKey>|<itemKey>`: the item's signature when the rule last started an agent on it.
  using AutomationMemory = std::map<std::string, std::string, std::less<>>;
  [[nodiscard]] std::string memoryKeyOf(std::string_view ruleKey, std::string_view itemKey);
  /// A start the planner chose: an item and the rule (or, for bulk, the task) that starts on it.
  struct PlannedStart { std::size_t item = 0; std::optional<std::size_t> rule; std::string task; };
  struct AutomationPlan { std::vector<PlannedStart> start; std::vector<PlannedStart> queued; };
  struct PlanInput {
      std::span<WorkItem const> items;            ///< In Work-view order (orderOf(..., Importance)).
      std::span<AutomationRule const> rules;      ///< Empty while the switch is paused.
      std::span<RuleState const> ruleStates;      ///< One per rule.
      AutomationMemory const& memory;
      std::span<std::string const> busyItems;     ///< Item keys with a pending StartAutomatic.
      std::span<std::string const> bulkItems;     ///< Item keys a bulk start still waits to start, in order.
      std::span<std::string const> bulkTasks;     ///< One per bulk item: the task, empty for suggested.
      Config const& config;                       ///< For suggestTask() on bulk items.
      std::size_t freeSlots = 0; };
  [[nodiscard]] AutomationPlan planAutomation(PlanInput const& input);
  ```
  Rules: an item with `agentRun` non-empty, or in `busyItems`, is skipped. Bulk items come first, in their own
  order; then rule matches in item order, the first rule in configuration order winning, skipping a Paused rule
  and a rule whose memory entry equals the item's signature. The first `freeSlots` go to `start`, the rest to
  `queued`. A bulk item whose suggested task is none, or whose chosen task does not apply, is dropped.

- [ ] **Step 1: Write the failing tests** (table-driven, `[automation]`), with `WorkItem` fixtures built the way
  `WorkItems_test.cpp` builds them:
  - `conditions match`: one row per condition. Failing CI matches `pipeline_failed`; a Draft with failing CI does
    not; Conflicting matches `conflicts`; ChangesRequested matches `changes_requested`; Passing+Approved matches
    `ready_to_merge`; a NoMrYet ticket matches `no_mr_yet`; a ticket never matches an MR condition.
  - `signature`: an MR with `headSha "abc"` → `sha:abc`; with no sha and `updatedWallMs 5` → `upd:5`; a ticket with
    status `In Progress` → `status:In Progress`.
  - `planner starts up to the free slots, queues the rest, in item order`.
  - `an item with a run is skipped`; `a busy item is skipped`.
  - `first matching rule wins`; `a paused rule is skipped and the next one may match`.
  - `an unchanged signature does not fire; a changed one does`.
  - `bulk items go first and ignore the memory`; `a bulk item no task applies to is dropped`.
  - `no free slots: everything is queued`.
- [ ] **Step 2: Run** `orchestrator_test "[automation]"`. Expected: FAIL to compile.
- [ ] **Step 3: Implement** `Automation.cpp`. The condition test is a `switch` on `AutomationCondition`, with
  `groupOf(mr) == WorkGroup::ReadyToMerge` for `ready_to_merge`.
- [ ] **Step 4: Run** `orchestrator_test "[automation]"`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(orchestrator): plan which items automation starts`.

### Task 4: The engine runs the planner

**Files:**
- Create: `core/EngineAutomation.cpp` (the engine's automation members), `core/EngineHarness.hpp` (test-only: the
  `Harness` class and the helpers it uses, moved verbatim from `Engine_test.cpp`), `core/EngineAutomation_test.cpp`
- Modify: `core/Engine.hpp`, `core/Engine.cpp` (`loadStore`, `endStep`, `step`'s visitor), `core/EnginePoll.cpp`
  (`onListAnswered` prunes the memory), `core/EngineDispatch.cpp` (an automatic dispatch's refusal),
  `core/Engine_test.cpp` (includes the harness), `CMakeLists.txt`, `view/DispatchForm.cpp` (its `RefusalTexts`
  move into core as `dispatchRefusalText()`; `view::refusalText()` returns it)

**Interfaces:**
- Consumes: Task 1 to 3.
- Produces (`Engine.hpp`):
  ```cpp
  inline constexpr std::string_view AutomationKey = "automation/state";
  inline constexpr uint32_t MaxRuleRefusals = 3;
  struct SetAutomation { AutomationSwitch to = AutomationSwitch::Paused; uint64_t monoMs = 0; };   // Input
  struct RunOnItems { std::vector<std::string> items; std::string task = {}; uint64_t monoMs = 0; }; // Input
  struct StartAutomatic { Dispatch request; };   // Effect: the driver mints session, wallMs and monoMs
  [[nodiscard]] std::string_view dispatchRefusalText(DispatchRefusal refusal) noexcept;
  // Engine, public:
  [[nodiscard]] AutomationSwitch automation() const noexcept;
  [[nodiscard]] bool hasAutomationRules() const noexcept;
  [[nodiscard]] bool isQueuedForAutomation(std::string_view itemKey) const noexcept;
  [[nodiscard]] std::size_t activeAutomaticRuns() const noexcept;
  ```
  Stored value: `{"switch":"on"|"paused","attempts":{"<memoryKey>":"<signature>"},"paused":["<ruleKey>"]}`,
  written whenever one of them changes; unreadable → the configured default, plus a start problem line.
- Behaviour:
  - `free slots = automationCapOf() − activeAutomaticRuns() − _automaticPending.size()`, never below 0.
  - For each planned start: build a `Dispatch` (repo, item, url, task: the rule's or bulk's or suggested, model,
    worker, permissions from the rule, `origin`, `originRule`), push the item key to `_automaticPending`, record
    the memory entry for a rule, emit `StartAutomatic`. The memory entry is written when the start is issued, so
    a refusal does not cause a retry within the same state either; only a failed *capacity* refusal removes it
    again.
  - In `dispatch()`, a request with `origin != User` removes its item from `_automaticPending` and never emits
    `DispatchRefused`. A capacity refusal (`AgentLimit`, `RepoLimit`, `AlreadyRunning`) removes the memory entry
    and is not counted. Any other refusal counts against the rule (bulk: a problem line only); a success resets
    the rule's count. The third refusal in a row pauses the rule and stores it.
  - Problem lines (in `rebuildProblems()`): `automation: <ruleKey> could not start on <label>: <refusal text>`
    for the last refusal of each rule, and `automation: <ruleKey> is paused after 3 refusals; switch automation
    off and on to resume it`.
  - A hooks check: a rule whose worker's `hooksFor(envSet)` is `Missing` refuses with "its worker's hooks are
    missing" (counted, no dispatch issued).
  - `SetAutomation{On}` from Paused clears the paused rules and the refusal counts. Paused: the queue empties;
    running automatic runs are left alone.
  - `RunOnItems` appends its items to the bulk list (an item already listed is not added twice).

- [ ] **Step 1: Move the harness.** Cut the `Harness` class and every helper it and the fixtures use (from the
  harness section through the fixture helpers before the first `TEST_CASE`) into `core/EngineHarness.hpp` inside
  `namespace orchestrator::test`, `inline` functions; `Engine_test.cpp` includes it.
- [ ] **Step 2: Run** `orchestrator_test`. Expected: 752 test cases pass, as before.
- [ ] **Step 3: Write the failing tests** in `EngineAutomation_test.cpp` (`[automation]`), with a configuration
  holding rules and a poll answered with MR lists, as `Engine_test.cpp`'s Work tests answer them:
  - `paused by default: a poll starts nothing`.
  - `switched on: a failing MR gets a StartAutomatic` with the rule's task, worker, model, `origin == Rule`,
    `originRule == "pipeline_failed/fix-ci"`, and the switch is stored under `automation/state`.
  - `the cap holds`: three matches, cap 2 → two `StartAutomatic`, the third `isQueuedForAutomation`.
  - `pending start is not issued twice`: a second step before the `Dispatch` arrives issues nothing.
  - `one attempt per state`: run it to Finished; another poll with the same head sha starts nothing; a new head
    sha starts again.
  - `memory survives a restart`: restart from `harness.stored()`; the unchanged item starts nothing.
  - `user runs do not count and are not blocked`.
  - `three refusals pause the rule`, with both problem lines; switching off and on resumes it.
  - `capacity refusal keeps the item queued` (`max_agents_per_repo` reached).
  - `removed rule leaves the queue`.
  - `bulk starts while paused and obeys the cap`.
  - `a poll prunes the memory of items no longer listed`.
- [ ] **Step 4: Run** `orchestrator_test "[automation]"`. Expected: FAIL.
- [ ] **Step 5: Implement** the members in `EngineAutomation.cpp`; `endStep()` calls `planAndStart(out)` before
  `drainQueue(out)`.
- [ ] **Step 6: Run** `orchestrator_test`. Expected: all pass.
- [ ] **Step 7: Commit** `feat(orchestrator): rules start agents on matching work items`.

### Task 5: An automatic run ends at Done

**Files:**
- Modify: `core/Engine.cpp` (`endsWhenDoneAt()`)
- Test: `core/EngineAutomation_test.cpp`

**Interfaces:** `endsWhenDoneAt(run)` returns `since` (no delay) when `run.origin != RunOrigin::User`, whatever
`end_done_after` says.

- [ ] **Step 1: Write the failing tests:** `automatic run ends when done` (report Done → the removal is issued at
  once, the run finishes as removed); `a user's run still waits for end_done_after`; `automatic run with dirty
  worktree ends kept` (status reports a change → Finished, fate kept); `an asking or errored automatic run is
  not ended`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `orchestrator_test`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(orchestrator): an automatic run ends itself when its agent is done`.

### Task 6: The driver starts automatic dispatches

**Files:**
- Modify: `guest/Driver.cpp` (`carryOut()`'s visitor; `inputOf` for the new view actions)
- Test: `guest/Driver_test.cpp`

**Interfaces:** On `StartAutomatic`, the driver copies the request, sets `session = formatUuidV4(_ids.next())`,
`wallMs` and `monoMs` from its clock, and queues it in `_replies` as an `Input`. It maps
`view::SetAutomationAction` to `SetAutomation` and `view::RunOnItemsAction` to `RunOnItems` (Task 7 defines both
actions; add the mapping in that task if this one lands first).

- [ ] **Step 1: Write the failing tests:** `automatic start is stepped as a dispatch` (the observer sees a
  `Dispatch` with a fresh session, origin Rule, after the batch); `automatic start runs with no tab served`.
- [ ] **Step 2: Run** `orchestrator_test "*automatic*"`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `orchestrator_test`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(orchestrator): the guest starts what automation asks for`.

### Task 7: The switch, the marks, and the queue on the dashboard

**Files:**
- Modify: `view/Keys.hpp` (`SetAutomationAction`, `RunOnItemsAction` in `UiAction`; `KeyCommand::ToggleAutomation`,
  `KeyCommand::RunOnGroup`; `WorkFocus::AutomationChip`), `view/Keys.cpp` (handler rows), `view/Actions.cpp`
  (command rows: both palette-only; `ToggleAutomation` Global, `RunOnGroup` Work), `view/WorkBoard.cpp` (the chip,
  `⚙ queued`), `view/Sections.cpp` (the `⚙` mark and the `started by` detail), `view/HitMap.hpp` if the chip's
  hit needs its value, `guest/Driver.cpp` (if Task 6 did not map the actions)
- Test: `view/WorkBoard_test.cpp`, `view/Sections_test.cpp`, `view/Palette_test.cpp`, `view/Keys_test.cpp`

**Interfaces:**
- `struct SetAutomationAction { AutomationSwitch to; };` `struct RunOnItemsAction { std::vector<std::string>
  items; std::string task; };`
- The chip, shown only while `engine.hasAutomationRules()`: `auto ▶ on` / `auto ⏸ paused`; ⏎ on it, or a click,
  gives `SetAutomationAction` with the other state.
- Palette: `ToggleAutomation`, label `switch automation on or off`, why-not when there are no rules: `no automation
  rules are configured`.
- Agents list: a run with `origin != User` draws `⚙` before its text (the run's own mark stays); its details add
  `started by`: `rule <when> → <task label>` or `a group start`.
- Work rows: an item `engine.isQueuedForAutomation(workItemKey(item))` shows `⚙ queued` where an agent's worker
  shows.

- [ ] **Step 1: Write the failing tests:** the chip's text in both states and its absence without rules; ⏎ on the
  chip and its click give `SetAutomationAction`; the palette lists the entry, dimmed with the reason when there are
  no rules; a Rule run's row text starts with `⚙` and its details hold `started by`; a queued item shows
  `⚙ queued`.
- [ ] **Step 2: Run** `orchestrator_test "[view]"`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `orchestrator_test`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(orchestrator): show and switch automation on the dashboard`.

### Task 8: Run a task on a group

**Files:**
- Create: `view/BulkDialog.hpp`, `view/BulkDialog.cpp`, `view/BulkDialog_test.cpp`
- Modify: `view/Dashboard.cpp` (open it from `KeyCommand::RunOnGroup`, draw it as the start dialog is drawn, route
  keys to it), `view/Keys.hpp` (the overlay kind), `CMakeLists.txt`

**Interfaces:**
```cpp
struct BulkDialogState {
    WorkGroup group; std::vector<std::string> items;   ///< The group's shown items' keys, in shown order.
    std::size_t choice = 0;                              ///< 0: each item's suggested task; i: tasks()[i-1].
    bool operator==(BulkDialogState const&) const = default; };
[[nodiscard]] std::optional<BulkDialogState> openBulkDialog(Engine const& engine, WorkGroup group,
                                                            std::span<WorkItem const> shown);
[[nodiscard]] std::size_t bulkCount(Engine const& engine, BulkDialogState const& dialog);   // items a task applies to
[[nodiscard]] std::vector<std::string> bulkLines(Engine const& engine, BulkDialogState const& dialog);
/// ←/→ step the choice among "suggested" and the tasks applying to any item; ⏎ gives RunOnItemsAction with
/// the applicable items; esc closes.
```
Text: the title is `Run a task on <group name>`. The lines are `Start <task label | the suggested task> on
<n> <merge requests | tickets | items>` and `each on its task's worker and model`, then the keys line
`←/→ task · ⏎ start · esc cancel`. With `n == 0`, ⏎ does nothing and the line reads `no item here takes that task`.

- [ ] **Step 1: Write the failing tests:** opening on a group of 4 failing MRs gives the suggested choice and
  count 4; stepping to `fix-ci` keeps 4 and to an issue task gives 0; ⏎ gives
  `RunOnItemsAction{items, "fix-ci"}`; esc closes; the palette's RunOnGroup is dimmed outside the Work view with
  `only in the Work view`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `orchestrator_test`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(orchestrator): start one task on every item of a Work group`.

### Task 9: Documentation, install and review

**Files:**
- Modify: `extensions/orchestrator/README.md` (an "Automation" section: the config block from the spec, the
  conditions table, the switch, the marks, ending at Done, running without a window)
- Modify: `docs/extensions.md` **through the index only**: write the HEAD blob plus the new section to a
  scratchpad file, `git hash-object -w`, `git update-index --cacheinfo 100644,<hash>,docs/extensions.md`,
  commit, then write the user's working copy merged with the section to the scratchpad and give them its path.

- [ ] **Step 1:** Write both documents.
- [ ] **Step 2:** Run `ctest --preset clang-debug -R "orchestrator|check_include_budget"`. Expected: all pass,
  `orchestrator_manifest_matches_corpus` included (no command changed).
- [ ] **Step 3: Commit** `docs(orchestrator): automation rules`.
- [ ] **Step 4:** A whole-branch review on Opus (`superpowers:requesting-code-review`) of the commits since
  `e424009e7`; fix what it confirms, one commit per fix.
- [ ] **Step 5:** Build the wasm and install into contour-dev as the earlier work did; tell the user to restart
  the daemon. No re-consent is needed: no command changed.
