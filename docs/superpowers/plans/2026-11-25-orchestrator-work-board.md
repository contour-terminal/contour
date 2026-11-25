# Orchestrator Work Board Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A **Work** view beside the dashboard's Agents view lists the user's open merge requests and Jira/forge
tickets in five groups with pipeline, review, conflicts, Jira status and agent; `⏎` or a double-click opens a start
dialog with a suggested task and a choosable worker; `n` starts a custom task at once; the search launcher goes.

**Architecture:** Bottom-up. The providers gain the fields the board needs (conflicts, update time, Jira status
and category, GitLab pipelines and failed jobs through two fixed REST calls per GitLab repository). A pure
`core/WorkItems` joins merge requests, tickets and runs into grouped, ordered items and suggests a task and a
worker. The engine polls the new commands (GitLab pipelines and failed jobs, the optional `in_progress_jql` query, one Jira key
lookup per poll), exposes what `WorkItems` needs, and lets a dispatch name its worker. The view adds
`view/WorkBoard`, `view/StartDialog`, `view/TaskBox` and `view/RowMenu`, switched with `w`, driven by keys and the
existing hit map; `view/Launcher` is deleted. Folds and the sort are kept as fold keys, so the store does not change.

**Tech Stack:** C++23, Catch2 (`orchestrator_test`, `vtextension_test`), glaze (orchestrator JSON), core-cpp's
`core::tui` (headless), the manifest-v2 corpus, `glab`/`acli`/`gh` for the recordings, Python for the scrub and
projection scripts.

**Spec:** `docs/superpowers/specs/2026-11-25-orchestrator-work-board-design.md` (approved, binding). Read it before
any task.

> **Spec amendment (controller's ruling, 2026-11-25).** The spec's one `glab api graphql` call per GitLab repository
> is replaced by **two fixed REST calls per GitLab repository per poll**: `glab api
> projects/:id/pipelines?per_page=100&order_by=id&sort=desc` (an MR's pipeline is the highest-id pipeline whose
> `sha` equals the MR's `sha`) and `glab api projects/:id/jobs?scope[]=failed&per_page=100` (failed job names,
> joined through that pipeline's id). A GraphQL query cannot be declared without changing the host's manifest
> grammar (no brace, space or quote may stand in a version-2 word), which is out of proportion for one extension.
> Approval stays inferred from `detailed_merge_status`. Still bounded, never one call per MR. Wherever this plan
> and the spec's **GitLab** paragraph differ, this note wins.
>
> **Further amendments (controller's rulings on the pre-flight scan, 2026-11-25).**
> 1. **No new `acli` command.** The spec's new `acli` command for `key in (…)` lookups and the `in_progress_jql`
>    query (with `--fields key`) is replaced by the existing `acli-issues` command with another `{jql}` (its argv
>    would be identical; Decision 3). Still one call per Jira source with `in_progress_jql` and one lookup per poll.
> 2. **No second-`⏎` override for an item an agent works on.** The spec's "asks for a second `⏎`" becomes: the
>    dialog says `⬢ <worker> already works on this`, and `⏎` shows the engine's `AlreadyRunning` refusal (dispatch
>    is unchanged per the spec's Non-goals; Decision 7).
> 3. **Screen-test sizes.** The grouped board is drawn at 50×20, 80×24, 120×40 and 160×40; the folded group, the
>    flat sort, the filter and the row menu at 80×24 and 120×40; the start dialog and the `n` box at 50×20, 80×24
>    and 120×40 — not every scene at all four sizes.

## Global Constraints

- **The user's uncommitted work.** `docs/extensions.md` and `cmake/ContourWasiCxx.cmake` carry the user's own
  uncommitted hunks. For the whole plan: never `git add -A`, `git add .`, `git add -u`, `git commit -a`, or a
  pathspec commit (`git commit -- <path>`); stage named paths only and run `git diff --cached --name-only` before
  every commit — it must list exactly the task's files. Never `git stash`, `git checkout -- <file>`,
  `git restore`, `git reset --hard`, a formatter or `sed -i` on either file. `cmake/ContourWasiCxx.cmake` is not
  edited by this plan. **`docs/extensions.md` is never edited by an implementer**: every change to it is made
  "via the WIP-preserving procedure the controller runs; the implementer writes the new text into
  `$S/work-board-extensions.md` instead" (`$S` is the session's scratchpad directory, which the controller names).
- **Never edit `vendor/core-cpp`.** Use what it offers (`core::base64::encode` from `<core/Base64.hpp>`,
  `core::tui::MouseEvent`).
- **Builds:** one at a time, from the repository root, 10-minute timeout, rerun on timeout. Git Bash:
  `CMAKE_BUILD_PARALLEL_LEVEL=2 cmake --build --preset clangcl-release -j 2 --target orchestrator_test` (PowerShell:
  `$env:CMAKE_BUILD_PARALLEL_LEVEL=2; cmake --build --preset clangcl-release -j 2 --target orchestrator_test`).
  Do not reconfigure `out/build/clangcl-release`; do not delete its `_deps`. Test binaries:
  `out/build/clangcl-release/bin/<target>.exe`.
- **Goldens.** Screens: `ORCHESTRATOR_UPDATE_GOLDENS=1 out/build/clangcl-release/bin/orchestrator_test.exe "golden
  screens"` writes `extensions/orchestrator/view/testdata/<name>.screen`; rerun without the variable → pass; read
  every new or changed `.screen` file before committing. Manifest: from **Git Bash** only,
  `cmake --build --preset clangcl-release -j 2 --target orchestrator_manifest` then
  `out/build/clangcl-release/bin/orchestrator_manifest.exe > extensions/orchestrator/contract/manifest-v2/valid/orchestrator.manifest`;
  then `cmake --build --preset clangcl-release -j 2 --target vtextension_test`, run
  `vtextension_test.exe "ExtensionConsent: the daemon question for the orchestrator"` and update
  `contract/manifest-v2/valid/orchestrator.consent.txt` to the text the failure prints; the diff of both goldens
  may hold only the task's command changes. `ctest --test-dir out/build/clangcl-release -R orchestrator` → all pass.
- **AGENT.md:** C++23; range-for only; `[[nodiscard]]`; Doxygen on new public API; `enum class : uint8_t` over
  `bool`, zero enumerator the negative/default; data-driven tables (one row per group, role, column, key, mouse
  kind, menu entry, poll list); `std::expected` for fallible API; constructor-complete objects; no `NOLINT` beyond
  the existing `glz::meta` `value` spelling; zero warnings; `clang-format -i` every C++ file touched; `.hpp` +
  `#pragma once`.
- **The orchestrator stays wasm32-wasi portable and free of Contour's `src/`.** After every task that touches
  `extensions/orchestrator/{core,view,guest}`: `python scripts/check-guest-portability.py` and
  `python scripts/check-include-budget.py` exit 0 (no `<regex>`, no exceptions, no `at(`, glaze only in `.cpp`).
- **Bounded polling (spec, Anti-goals, as amended):** per GitLab repository and poll at most two `glab api`
  calls (pipelines, failed jobs), one `acli` call per Jira source with `in_progress_jql`, and per poll one `acli` key lookup.
  Never a call per merge request.
- **Every configuration valid today stays valid.** New keys are optional; no key becomes reserved.
- **No real data in committed files.** Fixtures go through `harness/scrub-fixture.py`; hand-made data uses
  `example.atlassian.net`, `gitlab.example.com`, `group/project`, keys `PRJ-<n>`/`ABC-<n>`, users `user`, repos
  `contour`/`other`. `ctest --test-dir out/build/clangcl-release -R fixture-privacy` passes after every fixture
  change.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Subagent models (user's rule):** Haiku for Tasks 1, 4, 6, 8 (fully specified); Opus for Tasks 2, 3, 5, 7,
  9, 10, 11, 12, 13, 14 (recordings on the user's machine, engine wiring, views and goldens, docs procedure).
  Every review on Opus. A Haiku task that comes back blocked, or reaches a 4th fix round, is re-dispatched on Opus.

## Decisions (the builder's, per the spec's "Left to the builder", and where the code argued)

1. **Two GitLab REST calls (spec amendment above).** Declared commands `glab-pipelines`
   (`glab api projects/:id/pipelines?per_page=100&order_by=id&sort=desc`) and `glab-failed-jobs`
   (`glab api projects/:id/jobs?scope[]=failed&per_page=100`), both `@repos`, long timeout, no arguments; `glab`
   expands `:id` itself from the repository the command runs in. Checked against the host's version-2 argv rules
   (`src/vtextension/Manifest.cpp`, `readArgv` and `exactWords`): words are split at single spaces and refused only
   when empty, when they hold a quote, or a brace outside a whole-word `{name}`; `?`, `&`, `=`, `[`, `]` and `:`
   are accepted (the existing `gh-prs` `--jq` word already holds `[` `]`, `git -c status.displayCommentPrefix=false`
   holds `=`), and a line's head ends at its first `=`, which precedes the argv. No host change.
2. **Joining and falling back.** A listed MR's pipeline is the highest-`id` pipeline whose `sha` is the MR's `sha`
   (the REST MR list carries `sha`); its `status` gives `ci`; the failed jobs whose `pipeline.id` is that id give
   `failingChecks`. Review is inferred from `detailed_merge_status` by the existing `reviewStateOf()` (below), so
   a matched MR with a passing pipeline, no conflicts and no `not_approved`/`requested_changes` is Ready to merge.
   An MR with no matched pipeline (the pipelines call failed, its `sha` is not among the
   100 newest, a fork's pipeline runs in its own project, or the project has no CI) keeps today's inference and is
   `StateSource::Inferred`. A failed jobs call leaves `failingChecks` empty with a problem line; a failed pipelines
   call has its own problem line. **Conflicts, draft and update time come from the REST list** (`has_conflicts`,
   `draft`, `updated_at`, already in the recorded fixture). Review keeps today's `reviewStateOf()` for every GitLab
   MR (`not_approved` Required, `requested_changes` ChangesRequested, `mergeable` Approved, any other None): a
   status that says nothing about approval is never shown as `✓ approved` (pre-flight finding 8). Grouping is the
   same, since `groupOf` treats None and Approved alike for a Complete row.
3. **No new `acli` command.** The key lookup and the `in_progress_jql` query have exactly `acli-issues`' argv with
   another `{jql}`; a second identical declaration would only show the same line twice on the consent dialog.
   `acli-issues` gains `--fields key,summary,status,issuetype,updated` (spec). If the recording (Task 3) shows that
   `acli` refuses `key` in `--fields`, the list is `summary,status,issuetype,updated` (the key is always printed);
   the README notes which.
4. **MR → ticket key.** First token matching `[A-Z][A-Z0-9]+-[0-9]+` in the title (case-sensitive), else in the
   source branch **case-insensitively** (the orchestrator's own branches are `agent/prj-1234-…`, lower-cased by
   `itemSlug`), upper-cased. A token counts only at word boundaries (the byte before and after is not
   `[A-Za-z0-9]`) and only when its project is **covered**: the project of a key any Jira source listed this poll,
   or a project named in any Jira source's JQL (`project = X`, `project = "X"`, `project in (X, "Y")`, keyword
   case-insensitive). `UTF-8` and `SHA-256` therefore never link. Hand-written scanner (`<regex>` is not portable).
5. **Task roles.** Optional `role:` per task: `fix_ci`, `resolve_conflicts`, `address_review`, `work_on_issue`.
   Without it a task whose `id` spells a role has that role. Unknown `role` → `ConfigProblem::UnknownRole` (a new
   key, so no existing configuration breaks).
6. **`w`, `s`, `o` are bound but not added to `dashboardKeys()`.** Adding them would refuse today's configurations
   whose task `key` is one of them (the view fixtures' own `issue` task is keyed `w`), and task keys no longer act
   on the dashboard (README), so there is no clash.
7. **"An agent already works on it."** The engine's `AlreadyRunning` refusal stays (Non-goals: dispatch is
   unchanged). The dialog says `⬢ <worker> already works on this` before `⏎`, and `⏎` shows the refusal instead
   of dispatching; there is no second-`⏎` override.
8. **A worker that takes no permission profile** (Copilot) chosen for a task with its own `permissions` is refused
   (`DispatchRefusal::WorkerTakesNoProfile`), matching the configuration's `ProfileOnCopilot` rule through one
   shared helper `takesNoProfileFor(TaskDef const&, WorkerDef const&)`; the profile the dispatch carries does not
   count (the dialog always sends one, and a no-profile worker ignores it). The dialog marks such a worker
   `(no profile)`.
9. **GitLab without a matched pipeline** (Decision 2): `ChangeRequest::source = StateSource::Inferred`; such an
   MR is Ready to merge only when `detailed_merge_status` is `mergeable`.
10. **Folds and sort persist as fold keys** (`ToggleFoldAction`, `FoldsKey`): `work/needs-you`, `work/ready`,
    `work/in-review`, `work/no-mr`, `work/else`, each meaning "toggled from the group's default" (Everything else
    is folded by default); `work/sort-by-update` present means the flat sort. No store format change.
11. **Mouse motion.** While the Work view is shown each frame also sets `\033[?1003h` (any motion, which includes
    button-held motion); leaving it sets `\033[?1003l`. Motion with button 0 drags the scrollbar thumb; motion with
    button 3 (none held) hovers. Without motion events everything else works and the scrollbar takes clicks.
12. **Copy branch name** writes OSC 52 (`\033]52;c;<base64>\a`) into the next frame; the action bar says
    `copied <branch>`.
13. **Narrow widths** drop, in order: the Agent column's age (a `⬢ worker` stays), then Review text → glyph, then
    Pipeline text → glyph, then the Ticket column at under 60 columns.
14. **The start dialog keeps the permission profile** as its fifth setting (Task → Worker → Model → Perms →
    Note), defaulting as today (the task's profile, else the default); `FormField::Permissions` survives the
    launcher's removal.
15. **One rule for a run without `worker`:** `workerIdOf(Run const&)` (empty → `ImplicitWorkerId`, `"claude"`),
    used by the busy count and the Agent column alike.

## Review Focus

1. **A GitLab MR without a matched pipeline** (the pipelines call failed, or its `sha` is not among the 100
   newest) whose `detailed_merge_status` is
   `discussions_not_resolved`: must not land in Ready to merge (Task 5 `groupOf` row; Task 7 engine case).
2. **A stray token** — `UTF-8`, `SHA-256`, a key of a project no Jira source covers — in a title or branch: no
   link, `·` in the Jira column, and no key lookup issued for it (Task 5 scanner rows; Task 7 lookup argument).
3. **Copilot picked for a task with its own `permissions`:** refused with a reason, never run without the deny
   rules the user wrote (Task 8).
4. **A configuration whose task key is `w`, `s` or `o`** loads unchanged (Task 4).
5. **The list changes under an open dialog or menu** (a poll drops the merged MR): `⏎` dispatches nothing stale —
   the dialog shows the engine's refusal (Task 10) — and an open row menu closes (Task 12).

---

### Task 1: Update times, conflicts and checks from GitHub — Haiku

**Files:**
- Create: `extensions/orchestrator/core/Timestamps.hpp`, `extensions/orchestrator/core/Timestamps.cpp`,
  `extensions/orchestrator/core/Timestamps_test.cpp`
- Modify: `extensions/orchestrator/core/Model.hpp` (`Conflicts`, `StateSource`, `ChangeRequest` members)
- Modify: `extensions/orchestrator/core/GitHubJson.hpp`, `extensions/orchestrator/core/GitHub.cpp`
- Modify: `extensions/orchestrator/core/Commands.cpp` (`GhPrsArgv`)
- Modify: `extensions/orchestrator/core/testdata/github/prs-open.raw.json`, `prs-open.json`,
  `prs-mixed-checks.raw.json`, `prs-mixed-checks.json`, `prs-100.json`, `README.md`
- Modify: `extensions/orchestrator/contract/manifest-v2/valid/orchestrator.manifest`, `orchestrator.consent.txt`
- Modify: `extensions/orchestrator/CMakeLists.txt` (`orchestrator_core`, `orchestrator_test` sources)
- Test: `extensions/orchestrator/core/GitHub_test.cpp`

**Interfaces:**
- Produces:

```cpp
// Timestamps.hpp
/// @param text An ISO 8601 time with a zone: `YYYY-MM-DDTHH:MM:SS`, an optional `.` and 1–9 fraction digits, then
///        `Z`, `±HH:MM` or `±HHMM` (GitHub, GitLab and Jira spell it these ways).
/// @return Milliseconds since the Unix epoch (fraction truncated to ms); std::nullopt for anything else, or a
///         time before 1970.
[[nodiscard]] std::optional<uint64_t> unixMsOf(std::string_view text) noexcept;

// Model.hpp
/// Whether a change request conflicts with its base.
enum class Conflicts : uint8_t { None = 0, Conflicting };
/// Where a change request's CI and review state come from.
enum class StateSource : uint8_t
{
    Complete = 0, ///< The forge reported them directly.
    Inferred,     ///< Inferred from GitLab's single `detailed_merge_status` (no matched pipeline).
};
// ChangeRequest gains, after `closesIssues`:
Conflicts conflicts = Conflicts::None;           ///< Whether it conflicts with its base.
uint64_t updatedWallMs = 0;                      ///< When it was last updated, Unix ms; 0: unknown.
StateSource source = StateSource::Complete;      ///< Where @ref ci and @ref review come from.
```

  `JsonPullRequest` gains, last: `std::string mergeable; std::string updatedAt;` and its `glz::array` both, last.
  `GhPrsArgv`'s `--json` list gains `,mergeable,updatedAt`; the `--jq` row gains `,.mergeable,.updatedAt` after the
  `statusCheckRollup` array (still one word, no blank). Mapping: `mergeable == "CONFLICTING"` → `Conflicting`;
  `updatedWallMs = unixMsOf(updatedAt).value_or(0)`.

- [ ] **Step 1: Write the failing tests.** `Timestamps_test.cpp`, tag `[orchestrator][timestamps]`:

```cpp
TEST_CASE("unixMsOf reads the three forges' spellings", "[orchestrator][timestamps]")
{
    CHECK(unixMsOf("2026-11-13T15:08:47Z") == 1'794'582'527'000);
    CHECK(unixMsOf("2026-11-13T16:08:47.250+01:00") == 1'794'582'527'250);
    CHECK(unixMsOf("2026-11-13T16:08:47.000+0100") == 1'794'582'527'000);
    CHECK(unixMsOf("2026-11-13T12:00:00.123456Z") == 1'794'571'200'123);
    for (auto const bad: { ""sv, "2026-11-13T15:08:47"sv, "2026-11-13 15:08:47Z"sv, "2026-13-01T00:00:00Z"sv,
                           "2026-02-30T00:00:00Z"sv, "1969-12-31T23:59:59Z"sv, "2026-11-13T15:08:47+1"sv })
        CHECK_FALSE(unixMsOf(bad).has_value());
}
```

  `GitHub_test.cpp`: the recorded-fixture case gains `for (auto const& pr: prs.items) CHECK(pr.updatedWallMs != 0);`, and:

```cpp
TEST_CASE("GitHub: mergeable CONFLICTING is a conflict, and updatedAt the update time", "[github]")
{
    auto const listed = github::changeRequests(readFixture(FixtureRoot::Testdata, "github/prs-mixed-checks.json"),
                                               OutputExtent::Whole);
    REQUIRE(listed.has_value());
    // prs-mixed-checks: #2375 CONFLICTING (its #2376 row is skipped for its base, as before).
    REQUIRE(listed->items.size() == 1);
    CHECK(listed->items[0].conflicts == Conflicts::Conflicting);
    CHECK(listed->items[0].updatedWallMs == 1'794'571'200'000); // "2026-11-13T12:00:00Z"
    CHECK(listed->items[0].source == StateSource::Complete);
    // prs-open: every row MERGEABLE, so none conflicts.
    auto const open = github::changeRequests(readFixture(FixtureRoot::Testdata, "github/prs-open.json"), OutputExtent::Whole);
    REQUIRE(open.has_value());
    CHECK(std::ranges::none_of(open->items, [](auto const& pr) { return pr.conflicts == Conflicts::Conflicting; }));
}
```

  The existing "every projected row has as many members as the filter builds" case must now count 12.
- [ ] **Step 2:** Build `orchestrator_test`; run `"[timestamps]"` and `"[github]"` → fail.
- [ ] **Step 3: Implement** `Timestamps.*` (`std::chrono::year_month_day{…}.ok()` for the date, `sys_days`
  arithmetic; no clock read), the model members, the JSON members and mapping, the argv. Add the files to
  `CMakeLists.txt`.
- [ ] **Step 4: The fixtures** (there is no `jq` on this machine): confirm the value shapes once with
  `gh pr list --repo contour-terminal/contour --state open --limit 5 --json number,mergeable,updatedAt` (public,
  read-only). Then with a small Python script kept in `$S` (not committed): add `"mergeable"` and `"updatedAt"` to
  every object of both `*.raw.json` files (neither raw file has a `createdAt`: every recorded row gets
  `MERGEABLE` and `"2026-11-13T12:00:00Z"`; `prs-mixed-checks.raw.json`'s #2375 gets `CONFLICTING` and
  `"2026-11-13T12:00:00Z"`, its #2376 `UNKNOWN` and `"2026-11-13T11:00:00Z"`), and
  append the same two values, in that order, to each positional row of the matching projected file. Regenerate
  `prs-100.json` with `python -I -X utf8 extensions/orchestrator/harness/scale-fixture.py
  extensions/orchestrator/core/testdata/github/prs-open.json extensions/orchestrator/core/testdata/github/prs-100.json`.
  The GitHub README gains: "`mergeable` and `updatedAt` were added to the recorded replies by hand (their shapes
  checked against `gh` once): `MERGEABLE`, `CONFLICTING` or `UNKNOWN`, and an ISO 8601 time."
- [ ] **Step 5:** Regenerate the manifest and consent goldens (Global Constraints); the only change is the
  `gh-prs` line. Rebuild; `orchestrator_test.exe` (all), `ctest --test-dir out/build/clangcl-release -R
  "orchestrator|fixture-privacy"` → pass. Portability scripts → 0. `clang-format -i`.
- [ ] **Step 6: Commit** — the files above; `git diff --cached --name-only`; message
  `feat(orchestrator): read a pull request's conflicts and update time`.

### Task 2: GitLab — REST fields, pipelines and failed jobs — Opus

**Files:**
- Modify: `extensions/orchestrator/core/Model.hpp` (`PipelineInfo`, `FailedJob`, `ChangeRequest::headSha`)
- Modify: `extensions/orchestrator/core/GitLabJson.hpp`, `extensions/orchestrator/core/GitLab.hpp/.cpp`
- Modify: `extensions/orchestrator/core/Provider.hpp/.cpp` (five members per row)
- Modify: `extensions/orchestrator/core/Commands.cpp` (`glab-pipelines`, `glab-failed-jobs`)
- Modify: `extensions/orchestrator/harness/scrub-fixture.py` (shapes `glab-pipelines`, `glab-jobs`)
- Create: `extensions/orchestrator/core/testdata/gitlab/pipelines.json`, `jobs-failed.json` (recorded, scrubbed),
  `pipelines-hand.json`, `jobs-failed-hand.json` (hand-made)
- Modify: `extensions/orchestrator/core/testdata/gitlab/README.md`, `VERSION` (if glab changed)
- Modify: the manifest and consent goldens
- Test: `extensions/orchestrator/core/GitLab_test.cpp`, `extensions/orchestrator/core/Provider_test.cpp`,
  `extensions/orchestrator/core/Commands_test.cpp`

**Interfaces:**
- Consumes: `unixMsOf`, `Conflicts`, `StateSource` (Task 1).
- Produces:

```cpp
// Model.hpp (forge-neutral, so that Provider.hpp needs no forge header)
/// One pipeline of a project, as its pipeline list reports it.
struct PipelineInfo { uint64_t id = 0; std::string sha; CiState ci = CiState::None; bool operator==(PipelineInfo const&) const = default; };
/// One failed job, by the pipeline it ran in.
struct FailedJob { uint64_t pipelineId = 0; std::string name; bool operator==(FailedJob const&) const = default; };
// ChangeRequest gains: std::string headSha = {}; ///< Its head commit as the forge reported it; empty: unknown.

// GitLab.hpp
/// @param status A REST pipeline `status`.
/// @return success, skipped: Passing; failed, canceled: Failing; every other value (running, pending, created,
///         waiting_for_resource, preparing, manual, scheduled, canceling, unknown): Pending.
[[nodiscard]] CiState pipelineStateOf(std::string_view status) noexcept;
/// Maps `glab-pipelines`: a JSON array of `{id, sha, status, …}`; an entry without `id` or `sha` is skipped.
[[nodiscard]] std::expected<std::vector<PipelineInfo>, ParseError> pipelines(std::string_view stdoutText, OutputExtent extent);
/// Maps `glab-failed-jobs`: a JSON array of `{name, pipeline: {id}, …}`; names sanitised (displayText, MaxTitleChars).
[[nodiscard]] std::expected<std::vector<FailedJob>, ParseError> failedJobs(std::string_view stdoutText, OutputExtent extent);
/// Joins pipelines and failed jobs into the REST list. An MR whose `headSha` matches a pipeline
/// takes the highest-id one's ci and that pipeline's failed jobs (list order, each name once), keeps the review
/// the existing reviewStateOf() gave it, and is Complete; without a match it stays as changeRequests() mapped it and is Inferred.
/// @param listed The REST list's merge requests.
/// @param pipelines What pipelines() read; std::nullopt: the call failed.
/// @param jobs What failedJobs() read; std::nullopt: the call failed (failingChecks stay empty).
[[nodiscard]] std::vector<ChangeRequest> enriched(std::vector<ChangeRequest> listed,
                                                  std::optional<std::span<PipelineInfo const>> pipelines,
                                                  std::optional<std::span<FailedJob const>> jobs);

// GitLabJson.hpp: JsonMergeRequest gains bool hasConflicts = false; std::string updatedAt; std::string sha;
//   JsonPipeline { uint64_t id = 0; std::string sha; std::string status; };
//   JsonJobPipeline { uint64_t id = 0; }; JsonJob { std::string name; std::optional<JsonJobPipeline> pipeline; };

// Provider.hpp, Provider gains (GitHub's row: empty / nullptr):
std::string_view listPipelines = {};  ///< Lists the project's newest pipelines; empty: none.
std::string_view listFailedJobs = {}; ///< Lists the project's failed jobs; empty: none.
std::expected<std::vector<PipelineInfo>, ParseError> (*mapPipelines)(std::string_view, OutputExtent) = nullptr;
std::expected<std::vector<FailedJob>, ParseError> (*mapFailedJobs)(std::string_view, OutputExtent) = nullptr;
/// Joins what the two read into listChangeRequests' mapping; nullptr: none (the engine calls it through the row).
std::vector<ChangeRequest> (*enrich)(std::vector<ChangeRequest>, std::optional<std::span<PipelineInfo const>>,
                                     std::optional<std::span<FailedJob const>>) = nullptr;
```

  REST mapping: `conflicts = hasConflicts ? Conflicting : None`; `updatedWallMs = unixMsOf(updatedAt).value_or(0)`;
  `headSha = sha`; `source = StateSource::Inferred` (enriched() makes a matched one Complete); `review` stays
  `reviewStateOf(detailed_merge_status)` as today, so no merge-status member is needed. GitLab's provider row:
  `listPipelines = "glab-pipelines"`, `listFailedJobs = "glab-failed-jobs"`, `mapPipelines = &gitlab::pipelines`,
  `mapFailedJobs = &gitlab::failedJobs`, `enrich = &gitlab::enriched`.
  Command rows, after `glab-mrs`: `glab-pipelines` with argv `glab api projects/:id/pipelines?per_page=100&order_by=id&sort=desc`
  and `glab-failed-jobs` with argv `glab api projects/:id/jobs?scope[]=failed&per_page=100`; both `Timeout::Long`,
  `Root::Repos`, `Verification::Recorded`.

- [ ] **Step 1: Record (on the user's machine; read-only).** If `glab auth status` fails in a configured GitLab
  repository, stop and report BLOCKED. From that repository's checkout run both commands exactly as declared into
  `$S/pipelines-recorded.json` and `$S/jobs-recorded.json`. Add shapes to `scrub-fixture.py`: `glab-pipelines`
  (`sha` → `sha`, `ref` → `fmt:feature/branch-{n}`, `web_url` → `url:https://{host}/group/project/-/pipelines/{id}`,
  `status`/`source` keep, `user` → user); `glab-jobs` (`name` → `fmt:job-{n}`, `stage` keep, `ref` →
  `fmt:feature/branch-{n}`, `pipeline.sha` and `commit.id` → `sha`, every `commit.*` text, author and e-mail →
  `user`, every `web_url` → a url on `{host}`, `status`/`failure_reason` keep, `runner.*` text → `runner`), and their
  `HOSTS` entries. Scrub both into `core/testdata/gitlab/pipelines.json` and `jobs-failed.json`. Grep the results
  for the instance's host, the user's name and `@`: none (`ctest … -R fixture-privacy` must pass).
- [ ] **Step 2: Hand-make** `pipelines-hand.json` and `jobs-failed-hand.json` against `mrs-open.json`'s first
  three entries (iids 5752, 5746, 5744, each with its own `sha`): 5752 → pipeline 900 `failed` and pipeline 880
  `success` on its sha (the higher id wins), jobs `test-windows` and `lint` in 900 and `old` in 880; 5746 → 901
  `success`; 5744 → 902 `running`. Document both in the README's table.
- [ ] **Step 3: Write the failing tests** (`GitLab_test.cpp`, tag `[gitlab]`):

```cpp
TEST_CASE("GitLab: pipeline statuses by table", "[gitlab]")
{
    for (auto const [text, state]: std::array { std::pair { "success"sv, CiState::Passing }, { "skipped"sv, CiState::Passing },
             { "failed"sv, CiState::Failing }, { "canceled"sv, CiState::Failing }, { "running"sv, CiState::Pending },
             { "manual"sv, CiState::Pending }, { "new_value"sv, CiState::Pending } })
        CHECK(gitlab::pipelineStateOf(text) == state);
}
TEST_CASE("GitLab: the recorded pipelines and failed jobs map", "[gitlab]")
{
    auto const pipes = gitlab::pipelines(readFixture(FixtureRoot::Testdata, "gitlab/pipelines.json"), OutputExtent::Whole);
    auto const jobs = gitlab::failedJobs(readFixture(FixtureRoot::Testdata, "gitlab/jobs-failed.json"), OutputExtent::Whole);
    REQUIRE(pipes.has_value());
    REQUIRE(jobs.has_value());
    CHECK_FALSE(pipes->empty());
    CHECK(std::ranges::all_of(*pipes, [](PipelineInfo const& p) { return p.id != 0 && !p.sha.empty(); }));
    CHECK(std::ranges::all_of(*jobs, [](FailedJob const& j) { return j.pipelineId != 0 && j.name.starts_with("job-"); }));
}
TEST_CASE("GitLab: an MR's newest pipeline of its sha decides, and its failed jobs are named", "[gitlab]")
{
    auto listed = gitlab::changeRequests(readFixture(FixtureRoot::Testdata, "gitlab/mrs-open.json"), OutputExtent::Whole);
    auto const pipes = gitlab::pipelines(readFixture(FixtureRoot::Testdata, "gitlab/pipelines-hand.json"), OutputExtent::Whole);
    auto const jobs = gitlab::failedJobs(readFixture(FixtureRoot::Testdata, "gitlab/jobs-failed-hand.json"), OutputExtent::Whole);
    REQUIRE(listed.has_value());
    REQUIRE(pipes.has_value());
    REQUIRE(jobs.has_value());
    auto const mrs = gitlab::enriched(listed->items, std::span { *pipes }, std::span { *jobs });
    CHECK(mrs[0].ci == CiState::Failing);
    CHECK(mrs[0].failingChecks == std::vector<std::string> { "test-windows", "lint" });
    CHECK(mrs[1].ci == CiState::Passing);
    CHECK(mrs[2].ci == CiState::Pending);
    CHECK(mrs[0].review == listed->items[0].review); // reviewStateOf(), unchanged by the join
    CHECK(mrs[0].source == StateSource::Complete);
    CHECK(mrs[3].source == StateSource::Inferred); // no pipeline of its sha
    CHECK(gitlab::enriched(listed->items, std::nullopt, std::nullopt)[0].source == StateSource::Inferred);
    CHECK(gitlab::enriched(listed->items, std::span { *pipes }, std::nullopt)[0].failingChecks.empty());
}
TEST_CASE("GitLab: the REST list carries conflicts, update time and the head commit", "[gitlab]")
{
    auto const listed = gitlab::changeRequests(readFixture(FixtureRoot::Testdata, "gitlab/mrs-open.json"), OutputExtent::Whole);
    REQUIRE(listed.has_value());
    auto const byIid = [&](uint64_t iid) { return *std::ranges::find(listed->items, iid, &ChangeRequest::number); };
    CHECK(byIid(5696).conflicts == Conflicts::Conflicting); // detailed_merge_status `conflict`
    CHECK(byIid(5729).conflicts == Conflicts::Conflicting); // `draft_status`, has_conflicts true
    CHECK(byIid(5752).conflicts == Conflicts::None);
    CHECK(byIid(5696).updatedWallMs == unixMsOf("2026-11-20T10:24:50.48+01:00"));
    CHECK(std::ranges::all_of(listed->items, [](ChangeRequest const& mr) { return mr.updatedWallMs != 0 && !mr.headSha.empty(); }));
}
```

  `Commands_test.cpp`: `findCommand("glab-pipelines")` and `findCommand("glab-failed-jobs")` exist with the argv
  above, and `manifestText()` holds both lines as written. `Provider_test.cpp`: GitLab's row names both commands and
  has non-null `mapPipelines`, `mapFailedJobs` and `enrich`; GitHub's names none and has all three null.
- [ ] **Step 4:** Build; `orchestrator_test.exe "[gitlab]"` → fail.
- [ ] **Step 5: Implement** the JSON shapes and `glz::meta` (`has_conflicts`, `updated_at`, `sha`; the pipeline and
  job keys are already snake-free), the mappings, `enriched()`, the provider members (five, with `enrich`) and the
  two command rows.
- [ ] **Step 6:** Regenerate the manifest and consent goldens (two new command lines). Rebuild;
  `orchestrator_test.exe` (all); `vtextension_test.exe "[consent]"` and `vtextension_test.exe "[corpus]"` (the
  orchestrator manifest still parses under the host's unchanged rules); `ctest --test-dir
  out/build/clangcl-release -R "orchestrator|fixture-privacy"` → pass. Portability scripts → 0. `clang-format -i`.
- [ ] **Step 7: Commit** — the files above; message `feat(orchestrator): GitLab pipelines and failed jobs, two calls
  per repository`.

### Task 3: Jira — status, category and update time; `--fields` — Opus

**Files:**
- Modify: `extensions/orchestrator/core/Model.hpp` (`StatusCategory`, `Issue` members)
- Modify: `extensions/orchestrator/core/JiraJson.hpp`, `extensions/orchestrator/core/Jira.hpp/.cpp`
- Modify: `extensions/orchestrator/core/Commands.cpp` (`AcliIssuesArgv`)
- Modify: `extensions/orchestrator/core/testdata/jira/issues-open.json` (re-recorded), `README.md`, `VERSION`
  (if acli changed); `extensions/orchestrator/harness/scrub-fixture.py` (if `updated` needs a rule)
- Create: `extensions/orchestrator/core/testdata/jira/issues-work.json` (hand-made)
- Modify: the manifest and consent goldens
- Test: `extensions/orchestrator/core/Jira_test.cpp`, `extensions/orchestrator/core/Commands_test.cpp`

**Interfaces:**
- Consumes: `unixMsOf(std::string_view) -> std::optional<uint64_t>` (`core/Timestamps.hpp`, Task 1).
- Produces:

```cpp
// Model.hpp
/// Jira's status category: `statusCategory.key`, whatever the project calls the status.
enum class StatusCategory : uint8_t { Unknown = 0, ToDo, InProgress, Done };
// Issue gains, after url:
std::string status = {};                            ///< A Jira status's name, sanitised; empty for a forge issue.
StatusCategory category = StatusCategory::Unknown;  ///< Its category; Unknown for a forge issue.
uint64_t updatedWallMs = 0;                         ///< When it was last updated, Unix ms; 0: unknown.

// Jira.hpp
/// @param key A `statusCategory.key`: `new`, `indeterminate` or `done`.
/// @return ToDo, InProgress, Done; Unknown for anything else.
[[nodiscard]] StatusCategory statusCategoryOf(std::string_view key) noexcept;
```

  `JsonFields` gains `std::optional<JsonStatus> status; std::optional<std::string> updated;` with
  `JsonStatus { std::string name; std::optional<JsonStatusCategory> statusCategory; }`,
  `JsonStatusCategory { std::string key; }`. `AcliIssuesArgv` gains `--fields key,summary,status,issuetype,updated`
  after `{jql}`; if the recording shows `acli` refuses `key` in `--fields`, use
  `summary,status,issuetype,updated` (the key is always printed) and say so in the Jira README. The same command
  later serves the key lookup and the `in_progress_jql` query (Task 7), so no new `acli` command is declared. Status names go through `displayText(name, MaxTitleChars)`.

- [ ] **Step 1: Record (on the user's machine; read-only).** If `acli jira auth status` fails, stop and report
  BLOCKED. Run `acli jira workitem search --jql "assignee = currentUser() AND resolution = Unresolved" --fields
  key,summary,status,issuetype,updated --limit 100 --json > $S/issues-recorded.json` (if `key` is refused, the
  fallback list above). Scrub: `python -I -X utf8 extensions/orchestrator/harness/scrub-fixture.py --shape acli-issues
  $S/issues-recorded.json extensions/orchestrator/core/testdata/jira/issues-open.json`; replace any site-specific
  status name as the README already describes. Grep the result for the site and the user's name: none.
- [ ] **Step 2: Hand-make** `issues-work.json` (acli's shape, site `example.atlassian.net`): `PRJ-1234` "In
  Progress"/`indeterminate`, `PRJ-1240` "Selected for Development"/`new`, `PRJ-1300` "To Do"/`new`, `PRJ-1301` "In
  Review"/`indeterminate`, each with an `updated` such as `2026-11-13T12:00:00.000+0000`, and `PRJ-1302`
  `indeterminate` (no merge request names it, so the Work world's "No MR yet" group holds it), whose status name holds `\u001b[31m`. Document it in the README.
- [ ] **Step 3: Write the failing tests** (`Jira_test.cpp`, `[jira]`):

```cpp
TEST_CASE("Jira: status, category and update time", "[jira]")
{
    auto const listed = jira::issues(readFixture(FixtureRoot::Testdata, "jira/issues-work.json"), OutputExtent::Whole,
                                     IssueSourceContext { .site = "https://example.atlassian.net" });
    REQUIRE(listed.has_value());
    CHECK(listed->items[0].status == "In Progress");
    CHECK(listed->items[0].category == StatusCategory::InProgress);
    CHECK(listed->items[0].updatedWallMs == 1'794'571'200'000);
    CHECK(listed->items[1].category == StatusCategory::ToDo);
    CHECK_FALSE(listed->items[4].status.contains('\x1b'));
    CHECK(jira::statusCategoryOf("done") == StatusCategory::Done);
    CHECK(jira::statusCategoryOf("other") == StatusCategory::Unknown);
}
TEST_CASE("Jira: the recorded search carries status and update time", "[jira]")
{
    auto const listed = jira::issues(readFixture(FixtureRoot::Testdata, "jira/issues-open.json"), OutputExtent::Whole,
                                     IssueSourceContext { .site = "https://example.atlassian.net" });
    REQUIRE(listed.has_value());
    CHECK(std::ranges::all_of(listed->items, [](Issue const& issue) {
        return !issue.status.empty() && issue.category != StatusCategory::Unknown && issue.updatedWallMs != 0;
    }));
}
```

  `Commands_test.cpp`: `acli-issues`' argv contains `--fields` followed by the chosen list.
- [ ] **Step 4:** Build; `"[jira]"` → fail. **Step 5: Implement.** **Step 6:** goldens (the `acli-issues` line
  only); `orchestrator_test.exe` (all); `ctest … -R "orchestrator|fixture-privacy"` → pass; portability → 0;
  `clang-format -i`.
- [ ] **Step 7: Commit** — message `feat(orchestrator): read a Jira ticket's status and update time`.

### Task 4: Configuration — `in_progress_jql` and a task's `role` — Haiku

**Files:**
- Modify: `extensions/orchestrator/core/ConfigJson.hpp` (`JsonIssueSource::inProgressJql`, `JsonTask::role`)
- Modify: `extensions/orchestrator/core/Config.hpp/.cpp` (field tables, `IssueQuery`, `TaskDef`, problems,
  `describe()` texts)
- Modify: `extensions/orchestrator/core/IssueSources.hpp/.cpp` (`progressKey`)
- Create: `extensions/orchestrator/core/TaskRoles.hpp` (header-only table)
- Test: `extensions/orchestrator/core/Config_test.cpp`

**Interfaces:**
- Produces:

```cpp
// TaskRoles.hpp
/// What a task is for, as the start dialog suggests it.
enum class TaskRole : uint8_t { None = 0, FixCi, ResolveConflicts, AddressReview, WorkOnIssue };
struct TaskRoleRow { TaskRole role; std::string_view spelling; };
inline constexpr auto TaskRoleRows = std::array {
    TaskRoleRow { .role = TaskRole::FixCi, .spelling = "fix_ci" },
    TaskRoleRow { .role = TaskRole::ResolveConflicts, .spelling = "resolve_conflicts" },
    TaskRoleRow { .role = TaskRole::AddressReview, .spelling = "address_review" },
    TaskRoleRow { .role = TaskRole::WorkOnIssue, .spelling = "work_on_issue" },
};
/// @return The role @p spelling names; std::nullopt for none.
[[nodiscard]] constexpr std::optional<TaskRole> taskRoleNamed(std::string_view spelling) noexcept;

// Config.hpp
// IssueQuery gains: std::string inProgressJql = {}; ///< A Jira source's `in_progress_jql`; empty: none.
// TaskDef gains:    TaskRole role = TaskRole::None;  ///< `role`, else the role its id spells, else None.
// ConfigProblem gains, last: UnknownRole ///< A task's `role` no TaskRoleRows row spells.
// IssueSourceRow gains: std::string_view progressKey; ///< "in_progress_jql" for jira; empty for the forge.
```

  Rules: `in_progress_jql` is read only where the row's `progressKey` names it (else `UnknownKey`, like `jql` on a
  forge source); checked as Text; `(<jql>) AND (<in_progress_jql>)` longer than `MaxTextBytes` → `NotText` on
  `repos[i].issues[j].in_progress_jql`. `role` unknown → `UnknownRole` with the four spellings in its text.
  `dashboardKeys()` is **not** changed: adding `w`, `s` or `o` would refuse today's configurations whose task key is
  one of them (the view fixtures' `issue` task is keyed `w`), and task keys no longer act on the dashboard.

- [ ] **Step 1: Write the failing tests** (`Config_test.cpp`, `[config]`):

```cpp
TEST_CASE("a jira source may name an in_progress_jql", "[config]")
{
    // repos[0].issues[0] = { source: jira, jql: "assignee = currentUser()", site: "https://example.atlassian.net",
    //   in_progress_jql: 'status in ("In Progress", "Selected for Development")' }
    // → config->repos()[0].issueQueries[0].inProgressJql == that text.
    // SECTION "on a forge source it is an unknown key": error key "repos[0].issues[0].in_progress_jql", UnknownKey.
    // SECTION "too long together": 8192 bytes of in_progress_jql → NotText at that key.
}
TEST_CASE("a task's role is written, or spelled by its id", "[config]")
{
    // tasks: { id: fix_ci, … } → role FixCi; { id: mend, role: resolve_conflicts, … } → ResolveConflicts;
    // { id: other, … } → None; { id: x, role: fix_everything } → UnknownRole at "tasks[n].role".
}
TEST_CASE("task keys w, s and o stay valid", "[config]")
{
    // three tasks keyed "w", "s", "o" → Config::parse succeeds (Review Focus 4).
}
```

- [ ] **Step 2:** Build; `orchestrator_test.exe "[config]"` → the new cases fail.
- [ ] **Step 3: Implement** (field tables and their `matchesMirror` asserts, the `SourceKey` row for
  `in_progress_jql` using `progressKey`, `describe()` rows).
- [ ] **Step 4:** Rebuild; `orchestrator_test.exe` (all) → pass; portability → 0; `clang-format -i`.
- [ ] **Step 5: Commit** — message `feat(orchestrator): in_progress_jql and a task's role in the configuration`.

### Task 5: `core/WorkItems` — linking, de-duplication, groups and order — Opus

**Files:**
- Create: `extensions/orchestrator/core/WorkItems.hpp`, `extensions/orchestrator/core/WorkItems.cpp`,
  `extensions/orchestrator/core/WorkItems_test.cpp`
- Modify: `extensions/orchestrator/CMakeLists.txt`

**Interfaces:**
- Consumes: `ChangeRequest` with `conflicts`, `updatedWallMs`, `source` (`Conflicts::{None,Conflicting}`,
  `StateSource::{Complete,Inferred}`, Task 1) and `headSha` (Task 2); `Issue` with `status`, `category`,
  `updatedWallMs` (`StatusCategory::{Unknown,ToDo,InProgress,Done}`, Task 3); `Repo::issueQueries` with
  `IssueQuery::{source,text,site,inProgressJql}` (Task 4); `Run`, `Run::worker`, `countsAsActive` (`Run.hpp`);
  `ImplicitWorkerId` (`Config.hpp`); `isJiraKey` (`Naming.hpp`).
- Produces (namespace `orchestrator`):

```cpp
/// The Work board's groups, in the order they are shown.
enum class WorkGroup : uint8_t { NeedsYou = 0, ReadyToMerge, InReview, NoMrYet, EverythingElse };
/// Whether a group starts folded.
enum class FoldDefault : uint8_t { Open = 0, Folded };
struct WorkGroupRow { WorkGroup group; std::string_view foldKey; std::string_view name; FoldDefault fold; };
inline constexpr auto WorkGroupRows = std::array {
    WorkGroupRow { WorkGroup::NeedsYou, "work/needs-you", "Needs you", FoldDefault::Open },
    WorkGroupRow { WorkGroup::ReadyToMerge, "work/ready", "Ready to merge", FoldDefault::Open },
    WorkGroupRow { WorkGroup::InReview, "work/in-review", "In review", FoldDefault::Open },
    WorkGroupRow { WorkGroup::NoMrYet, "work/no-mr", "No MR yet", FoldDefault::Open },
    WorkGroupRow { WorkGroup::EverythingElse, "work/else", "Everything else", FoldDefault::Folded },
};
/// The fold key whose presence means the flat sort by last update. A group's fold key held by the engine means
/// "toggled from the group's FoldDefault"; folds and sort persist through ToggleFoldAction, with no store change.
inline constexpr std::string_view SortByUpdateKey = "work/sort-by-update";

/// Whether a ticket or forge issue is being worked on.
enum class Progress : uint8_t { NotStarted = 0, InProgress };
/// An issue as a repository's sources listed it, with its progress decided.
struct ListedIssue { Issue issue; Progress progress = Progress::NotStarted; bool operator==(ListedIssue const&) const = default; };

/// One row of the Work board.
struct WorkItem
{
    WorkGroup group = WorkGroup::EverythingElse;
    std::string repo;                            ///< The repository it belongs to (configured name).
    std::optional<ChangeRequest> changeRequest;  ///< An MR row's merge request; empty for a ticket row.
    std::optional<Issue> ticket;                 ///< The linked ticket (MR row) or the ticket itself.
    std::string ticketKey;                       ///< The linked or own key (Jira key, or `#<n>`); empty: none.
    ItemRef item;                                ///< What a dispatch works on: ChangeRequestRef, JiraRef or IssueRef.
    uint64_t updatedWallMs = 0;                  ///< The MR's, else the ticket's update time.
    std::string agentRun;                        ///< An active run on it (or on its linked ticket); empty: none.
    std::string agentWorker;                     ///< That run's worker id; empty: none.
    bool operator==(WorkItem const&) const = default;
};

/// One repository's lists, in configuration order.
struct RepoWork { std::string_view repo; std::span<ChangeRequest const> changeRequests; std::span<ListedIssue const> issues; };
/// What workItems() joins.
struct WorkInput
{
    std::span<RepoWork const> repos;            ///< In configuration order: a ticket listed twice is the first's.
    std::span<Issue const> linkedTickets;       ///< The key lookup's tickets (Engine::linkedTickets()).
    std::span<std::string const> jiraProjects;  ///< jiraProjectsOf().
    std::span<Run const> runs;                  ///< Every run; only active ones (countsAsActive) count.
};

/// @return The worker @p run runs on: Run::worker, or ImplicitWorkerId (`claude`) when it is empty (Decision 15).
[[nodiscard]] std::string_view workerIdOf(Run const& run) noexcept;
/// @return The item's identity across rebuilds: `<repo>/!<n>` for an MR row, `<repo>/<JIRA-KEY>` or `<repo>/#<n>`
///         for a ticket row; what the view keeps a selection and an open menu on.
[[nodiscard]] std::string workItemKey(WorkItem const& item);

/// @return The covered Jira projects (a key links only into one of them; see the scanner below): the project part of every JiraRef among @p listed's issues, and
///         every project the JQL of @p repos' Jira queries names; upper case, sorted, unique. Takes what the engine
///         holds, so nothing is copied.
[[nodiscard]] std::vector<std::string> jiraProjectsOf(std::span<Repo const> repos, std::span<RepoWork const> listed);
/// @return The first covered Jira key in @p title (case-sensitive), else in @p branch (case-insensitive, upper-cased).
[[nodiscard]] std::optional<std::string> linkedJiraKey(std::string_view title, std::string_view branch,
                                                       std::span<std::string const> projects);
/// @return The forge issues of its own repository @p mr names: closesIssues, then each `#<digits>` in its title
///         (at a word boundary), each once, in that order.
[[nodiscard]] std::vector<uint64_t> namedForgeIssues(ChangeRequest const& mr);
/// @return @p issue's progress: a forge issue (IssueRef) is InProgress; a Jira ticket is InProgress when
///         @p inProgressKeys holds its key, or — @p inProgressKeys absent — when its category is InProgress.
[[nodiscard]] Progress progressOf(Issue const& issue, std::optional<std::span<std::string const>> inProgressKeys) noexcept;
/// @return The group of an MR row, the first match of the spec's table (algorithm below).
[[nodiscard]] WorkGroup groupOf(ChangeRequest const& mr) noexcept;
/// @return NoMrYet for an in-progress ticket no MR names; EverythingElse otherwise.
[[nodiscard]] WorkGroup groupOf(Progress progress) noexcept;
/// @return Every MR, and every ticket or issue no MR names, de-duplicated by key, in groups.
[[nodiscard]] std::vector<WorkItem> workItems(WorkInput const& input);
/// How the board orders its items.
enum class WorkSort : uint8_t { Importance = 0, LastUpdate };
/// @return Indices into @p items: by group then newest first (Importance), or newest first (LastUpdate); ties
///         by repository order, then MR rows before ticket rows, then MR number / key ascending.
[[nodiscard]] std::vector<std::size_t> orderOf(std::span<WorkItem const> items, std::span<std::string const> repoOrder, WorkSort sort);
/// @return Whether @p item matches @p filter, case-folded: `!<n>`/`#<n>`/`<n>` of its MR, its ticket key, title,
///         repository or URL contains it; an empty filter matches everything.
[[nodiscard]] bool matchesFilter(WorkItem const& item, std::string_view filter);
```

  `groupOf(ChangeRequest)` (the spec's matching order; an Inferred GitLab MR — no matched pipeline — is Ready only
  when GitLab says `mergeable`, i.e. review Approved):

```
if readiness == Draft                                   -> EverythingElse
if ci == Failing || review == ChangesRequested
   || conflicts == Conflicting                          -> NeedsYou
ready = (ci == Passing || ci == None) && (review == Approved || review == None)
if source == Inferred: ready = review == Approved       // only `mergeable` says Approved
if ready                                                -> ReadyToMerge
                                                        -> InReview
```

  Scanner (no `<regex>`): walk the text; a token starts at an upper-case letter (title) / ASCII letter (branch)
  whose preceding byte is not `[A-Za-z0-9]`; it is `[A-Z][A-Z0-9]+` (letters folded for the branch), `-`, one or
  more digits, followed by end or a byte not `[A-Za-z0-9]`; accept when `isJiraKey(upper)` and its project is in
  @p projects. JQL scan: case-insensitive `project`, blanks, then `=` and one name (quotes stripped) or `in`,
  blanks, `(` and a comma list up to `)`; names upper-cased.
  `workItems`: (1) MR rows: every MR of every repository; `ticketKey`/`ticket` from `linkedJiraKey` — the ticket
  looked up first among every repository's listed issues, then `linkedTickets` — else from the first named forge
  issue (`#<n>`, its `ListedIssue` in the same repository). (2) Ticket rows: every listed issue no MR named, keyed
  by Jira key or (repo, number), kept in the first repository that lists it; group by `groupOf(progress)`.
  (3) Agents: an active run of the same repository whose `item` equals the row's `item` (or, for an MR row, its
  linked ticket's `JiraRef`/`IssueRef`); `agentWorker = workerIdOf(run)`.

- [ ] **Step 1: Write the failing tests** (`WorkItems_test.cpp`, tag `[orchestrator][work]`), each a table:

```cpp
TEST_CASE("WorkItems: a key links only at word boundaries and in a covered project", "[orchestrator][work]")
{
    auto const projects = std::vector<std::string> { "ABC", "PRJ" };
    CHECK(linkedJiraKey("PRJ-1234: fix it", "", projects) == "PRJ-1234");
    CHECK(linkedJiraKey("fix it", "agent/prj-1234-20261113-150847", projects) == "PRJ-1234");
    CHECK(linkedJiraKey("ABC-7 then PRJ-1", "", projects) == "ABC-7");
    CHECK_FALSE(linkedJiraKey("UTF-8 handling", "", projects));        // Review Focus 2
    CHECK_FALSE(linkedJiraKey("SHA-256 sums", "", projects));
    CHECK_FALSE(linkedJiraKey("XPRJ-1234", "", projects));
    CHECK_FALSE(linkedJiraKey("OTHER-12", "", projects));              // not covered
    CHECK_FALSE(linkedJiraKey("prj-1234 in a title", "", projects));   // titles are case-sensitive
}
TEST_CASE("WorkItems: the covered projects come from listed keys and the JQL", "[orchestrator][work]")
{
    auto const repos = std::vector<Repo> { { .name = "contour", .issueQueries = {
        { .source = "jira", .text = R"(project = "abc" AND assignee = currentUser())", .site = "https://example.atlassian.net" },
        { .source = "jira", .text = "PROJECT in (Xyz, \"QQ\") ORDER BY updated", .site = "https://example.atlassian.net" } } } };
    auto const issues = std::vector<ListedIssue> { { .issue = { .item = JiraRef { "PRJ-1" }, .title = "t", .url = "u" } } };
    auto const listed = std::vector<RepoWork> { { .repo = "contour", .changeRequests = {}, .issues = issues } };
    CHECK(jiraProjectsOf(repos, listed) == std::vector<std::string> { "ABC", "PRJ", "QQ", "XYZ" });
}
TEST_CASE("WorkItems: an MR's group is the first row of the spec's table that matches", "[orchestrator][work]")
{
    // Rows (all else: Ready, ci Passing, review Approved, no conflicts, Complete):
    //   Draft + ci Failing → EverythingElse;  ci Failing → NeedsYou;  ChangesRequested → NeedsYou;
    //   Conflicting + Approved → NeedsYou;  Passing + Approved → ReadyToMerge;  None + None → ReadyToMerge;
    //   Pending + Approved → InReview;  Passing + Required → InReview;
    //   Inferred, ci None, review None (discussions_not_resolved) → InReview   (Review Focus 1)
    //   Inferred, review Approved (mergeable) → ReadyToMerge
}
TEST_CASE("WorkItems: progress by category, by query, and forge issues always", "[orchestrator][work]")
{
    // JiraRef ticket, category InProgress, no keys → InProgress; same with keys {"PRJ-9"} → NotStarted;
    // category ToDo with keys {"PRJ-1240"} and key PRJ-1240 → InProgress; IssueRef → InProgress.
}
TEST_CASE("WorkItems: a named ticket shows on its MR, not alone", "[orchestrator][work]")
{
    // repo contour: MR #2375 "PRJ-1234: …" Failing; MR #2380 "fix #77" Passing+Approved; listed issues:
    //   PRJ-1234 (InProgress), PRJ-1300 (NotStarted), IssueRef 77, IssueRef 78 (forge: InProgress).
    // → items: MR #2375 NeedsYou ticketKey "PRJ-1234"; MR #2380 ReadyToMerge ticketKey "#77";
    //   ticket IssueRef 78 NoMrYet; ticket PRJ-1300 EverythingElse; no row for PRJ-1234 or #77 alone.
}
TEST_CASE("WorkItems: a ticket listed by two repositories belongs to the first", "[orchestrator][work]")
{
    // repos contour, other both list PRJ-1240 (InProgress) → one item, repo == "contour".
    // An MR of "other" naming PRJ-1240 links it (any source's project), and no ticket row remains.
}
TEST_CASE("WorkItems: a key the sources did not list is found among the looked-up tickets", "[orchestrator][work]")
{
    // MR "PRJ-1999 …", no listed PRJ-1999, linkedTickets has PRJ-1999 "In Review" → ticket->status == "In Review".
    // Without it in linkedTickets → ticketKey "PRJ-1999", ticket empty.
}
TEST_CASE("WorkItems: an active run on the item or its ticket shows its worker", "[orchestrator][work]")
{
    // active run on ChangeRequestRef{2375}, worker "claude-work" → agentWorker "claude-work";
    // active run on JiraRef{"PRJ-1234"} with worker "" and the MR linking PRJ-1234 → agentWorker == "claude"
    // (workerIdOf), even when the configured workers' first is another; a Finished run → none.
}
TEST_CASE("WorkItems: an item's key is stable and distinct", "[orchestrator][work]")
{
    // MR #2375 of contour → "contour/!2375"; ticket PRJ-1300 → "contour/PRJ-1300"; forge issue 78 → "contour/#78";
    // the same item built twice gives the same key.
}
TEST_CASE("WorkItems: order by importance or by last update", "[orchestrator][work]")
{
    // Importance: group order, newest first inside; LastUpdate: newest first across groups; ties as documented.
}
TEST_CASE("WorkItems: the filter matches number, key, title, repository and URL", "[orchestrator][work]")
{
    // "!812", "#2375", "2375", "prj-1234", "flaky", "other", the MR's exact URL → match; "nothing" → no match.
}
```

- [ ] **Step 2:** Add the files to `CMakeLists.txt`; build; `orchestrator_test.exe "[work]"` → fail.
- [ ] **Step 3: Implement** as the Interfaces block says.
- [ ] **Step 4:** Rebuild; `orchestrator_test.exe "[work]"` and all → pass; portability → 0; `clang-format -i`.
- [ ] **Step 5: Commit** — message `feat(orchestrator): join merge requests, tickets and runs into work items`.

### Task 6: Task suggestion and the worker default — Haiku

**Files:**
- Modify: `extensions/orchestrator/core/WorkItems.hpp/.cpp`, `extensions/orchestrator/core/WorkItems_test.cpp`

**Interfaces:**
- Consumes: `TaskRole::{None,FixCi,ResolveConflicts,AddressReview,WorkOnIssue}` and `TaskDef::role`
  (`core/TaskRoles.hpp`, `Config.hpp`, Task 4); `WorkItem` (its `changeRequest`, `ticket`, `item`) and
  `workerIdOf(Run const&)` (empty `Run::worker` → `ImplicitWorkerId`, `"claude"`) (`core/WorkItems.hpp`, Task 5);
  `Config::tasks()`, `findTask()`, `workers()`, `workerFor()`, `appliesToOf()`; `countsAsActive` (`Run.hpp`).
- Produces:

```cpp
/// The task the start dialog suggests, and why.
struct TaskSuggestion
{
    TaskDef const* task = nullptr; ///< nullptr: no task applies to the item.
    std::string reason;            ///< Shown beside it.
};
/// First match: ci Failing → FixCi, reason `pipeline fails: <failing checks, ", "-joined>` (or `pipeline fails`);
/// Conflicting → ResolveConflicts, `conflicts with <base>`; ChangesRequested → AddressReview, `changes requested`;
/// a ticket row → WorkOnIssue, `ticket in progress` / `ticket`; then the first task that applies to the item,
/// reason `first task for a merge request` / `first task for a ticket`. A role no applicable task has falls
/// through to the next rule.
[[nodiscard]] TaskSuggestion suggestTask(Config const& config, WorkItem const& item);
/// @return How many active runs (countsAsActive) run on @p workerId, each counted by workerIdOf() (Decision 15).
[[nodiscard]] std::size_t activeRunsOn(Config const& config, std::span<Run const> runs, std::string_view workerId) noexcept;
/// @return The task's own worker; when it runs at least one agent, the worker with the fewest active runs, ties
///         in configuration order.
[[nodiscard]] WorkerDef const& defaultWorker(Config const& config, std::span<Run const> runs, std::string_view taskId);
```

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST_CASE("WorkItems: the suggested task follows the row's state", "[orchestrator][work][suggest]")
{
    // tasks: fix_ci (pr), mend (pr, role resolve_conflicts), address_review (pr), issue (issue, role work_on_issue)
    // Failing with checks {"test-windows"} → fix_ci, reason "pipeline fails: test-windows";
    // Failing and Conflicting → fix_ci; Conflicting base "master" → mend, "conflicts with master";
    // ChangesRequested → address_review, "changes requested"; Jira ticket row → issue, "ticket in progress";
    // Pending → first pr task (fix_ci), "first task for a merge request";
    // no task with role FixCi configured and Failing → first pr task.
}
TEST_CASE("WorkItems: the worker defaults to the task's, else the least busy", "[orchestrator][work][suggest]")
{
    // workers a, b, c; task's worker a.
    // no runs → a; one active run on a → b (b, c both 0: configuration order);
    // runs: a×1, b×1 → c; a×2, b×1, c×1 → b; a Finished run on a counts nothing → a.
    // implicit-only config: a run with worker "" counts for "claude"; defaultWorker is still "claude".
    // workers x (first), claude: a run with worker "" counts for "claude", not for x.
}
```

- [ ] **Step 2–4:** build → fail; implement; rebuild → `"[work]"` and all pass; portability; `clang-format -i`.
- [ ] **Step 5: Commit** — message `feat(orchestrator): suggest a task and a worker for a work item`.

### Task 7: The engine polls pipelines and failed jobs, the in-progress queries and the key lookup — Opus

**Files:**
- Modify: `extensions/orchestrator/core/Engine.hpp` (`RepoState`, accessors), `extensions/orchestrator/core/EnginePoll.cpp`
  (`issueLists`, `onListResult`, `onListAnswered`, `parsePollTag`/list names), `extensions/orchestrator/core/Engine.cpp`
  (`RepoState` construction)
- Create: `extensions/orchestrator/core/EngineWork.cpp` (`Engine::workItems()`)
- Modify: `extensions/orchestrator/CMakeLists.txt`
- Test: `extensions/orchestrator/core/Engine_test.cpp`

**Interfaces:**
- Consumes (exact signatures in the tasks that made them):
  - Task 2 (`Model.hpp`, `Provider.hpp`): `PipelineInfo`, `FailedJob`; the Provider row's `listPipelines`,
    `listFailedJobs` (command names), `mapPipelines(std::string_view, OutputExtent)`,
    `mapFailedJobs(std::string_view, OutputExtent)` (both `std::expected<std::vector<…>, ParseError>`), and
    `enrich(std::vector<ChangeRequest>, std::optional<std::span<PipelineInfo const>>,
    std::optional<std::span<FailedJob const>>) -> std::vector<ChangeRequest>`; null on GitHub's row.
  - Task 4 (`Config.hpp`): `IssueQuery::inProgressJql` (empty: none).
  - Task 5 (`WorkItems.hpp`): `ListedIssue { Issue issue; Progress progress; }`, `progressOf(Issue const&,
    std::optional<std::span<std::string const>>)`, `jiraProjectsOf(std::span<Repo const>, std::span<RepoWork const>)`,
    `linkedJiraKey(title, branch, projects) -> std::optional<std::string>`, `RepoWork`, `WorkInput`,
    `workItems(WorkInput const&) -> std::vector<WorkItem>`.
  - Existing: `jira::issues(text, extent, IssueSourceContext)`, `ListLimit`, `isJiraKey`.
- Produces:

```cpp
// Engine (public):
/// @return The repository's issues from every source, de-duplicated by URL, with their progress.
[[nodiscard]] std::span<ListedIssue const> listedIssues(std::string_view repo) const noexcept;
/// @return The tickets the last key lookup found: those MRs name that no source listed.
[[nodiscard]] std::span<Issue const> linkedTickets() const noexcept;
/// @return The Work board's items, from the lists, the lookup and the runs (EngineWork.cpp).
[[nodiscard]] std::vector<WorkItem> workItems() const;
// RepoState gains:
std::vector<ChangeRequest> listedChangeRequests;     ///< The list as mapped, before enrichment.
std::optional<std::vector<PipelineInfo>> pipelines;  ///< The last pipelines; nullopt: none or failed.
std::optional<std::vector<FailedJob>> failedJobs;    ///< The last failed jobs; nullopt: none or failed.
std::vector<std::optional<std::vector<std::string>>> inProgressByQuery; ///< Per query: the in-progress keys; nullopt: not asked or failed.
std::vector<ListedIssue> issues;                              ///< listedIssues().
// Engine gains (private):
std::vector<Issue> _linkedTickets;                            ///< linkedTickets(); kept across a failed lookup.
/// Whether this poll's key lookup was issued; reset to NotIssued when a poll starts.
enum class LookupPhase : uint8_t { NotIssued = 0, Issued };
LookupPhase _lookupPhase = LookupPhase::NotIssued;
// ShownLists (EnginePoll.cpp, what `shownOf` compares to bump `_revision`) gains the repo's `issues`.
```

  Poll lists (in `EnginePoll.cpp`'s anonymous namespace, beside `PrsList`): `PipelinesList = "pipelines"`, `FailedJobsList = "failed-jobs"`,
  `ProgressList = "progress/"` (+ query index, as `IssuesList`), `LinkedList = "jira-keys"`.
  Behaviour:
  - `issueLists`: after `PrsList`, when the provider names `listPipelines` / `listFailedJobs`, issue
    `pipelines` and `failed-jobs` (no arguments). For each Jira query with `inProgressJql`, issue `acli-issues` with
    `jql = std::format("({}) AND ({})", query.text, query.inProgressJql)` under `progress/<k>`. All count in
    `_pollOutstanding`.
  - `onListResult`: `PrsList` stores `listedChangeRequests`; `PipelinesList` stores `pipelines` (failure →
    nullopt and the problem line `<repo>: the pipeline list could not be read; CI and review are inferred from
    GitLab's merge status`); `FailedJobsList` stores `failedJobs` (failure → nullopt and `<repo>: the failed job
    list could not be read; failing jobs are not named`); each then sets `changeRequests =
    provider->enrich(listedChangeRequests, pipelines, failedJobs)` when the row's `enrich` is not null
    (`changeRequests` as before otherwise) — through the row, never `gitlab::` by name. `ProgressList` stores the keys of the listed items
    (failure → nullopt, problem line `<repo>: the Jira in-progress query "<in_progress_jql>" failed; tickets in
    progress are those Jira counts as in progress`). After any issue or progress result the repo's `issues` are
    rebuilt: de-duplicated as today, each with `progressOf(issue, <its query's keys>)` (an issue listed by several
    queries takes InProgress if any says so). `unclaimed` is unchanged.
  - When the poll's other lists are all answered (`_pollOutstanding` reaches 0) and the lookup was not issued this
    poll: collect, over every repository's `changeRequests`, `linkedJiraKey(title, head, jiraProjectsOf(…))` keys no
    `listedIssues` holds; sorted, unique, at most `ListLimit`; if any and a repository has a Jira query, issue
    `acli-issues` in the first such repository with `jql = "key in (" + keys joined by ", " + ")"` under
    `poll/<repo>/jira-keys` and count it in `_pollOutstanding`; the poll ends (backoff, drain) once it answered.
    Its result replaces `_linkedTickets` (mapped with that query's site); a failure keeps the previous ones and
    adds a problem line. A change of `_linkedTickets` bumps `_revision`, and so does a change of a repo's `issues`
    (they join `ShownLists`), so the Work board, which rebuilds on `revision()`, sees progress-only changes.
  - **Failures of the new lists only write their own problem line** (pipelines, failed jobs, progress, key
    lookup): unlike today's `failed()`, they leave `_pollResult` and `_authCheck` alone, so they neither back the
    poll off nor ask for a forge login check (the spec: they "fall back").
  - `workItems()` builds `WorkInput` from the repositories in configuration order and calls
    `orchestrator::workItems(input)` — qualified, because the member of the same name hides the free function.

- [ ] **Step 1: Write the failing tests** (`Engine_test.cpp`, reuse its fixture helpers; configurations hand-written
  with `example.atlassian.net`):

```cpp
TEST_CASE("a GitLab poll lists pipelines and failed jobs once per repository", "[orchestrator][engine][gitlab][work]")
{
    // A GitLab repo: the poll's RunCommands hold exactly one "glab-pipelines" and one "glab-failed-jobs" (cwd: the
    // repo) beside one "glab-mrs"; a GitHub repo gets neither. Answered with mrs-open.json, pipelines-hand.json and
    // jobs-failed-hand.json → changeRequests(repo)[0].ci == Failing, .failingChecks == {"test-windows","lint"}.
}
TEST_CASE("a failed pipeline list leaves inferred rows and a problem line", "[orchestrator][engine][gitlab][work]")
{
    // glab-pipelines exits 1 → every changeRequests(repo)[i].source == StateSource::Inferred; problems() holds
    // "the pipeline list could not be read"; workItems(): an MR whose detailed_merge_status is
    // discussions_not_resolved is in InReview, not ReadyToMerge (Review Focus 1).
    // SECTION "a failed job list only loses the names": glab-failed-jobs exits 1 → [0].ci == Failing,
    // failingChecks empty, problems() holds "the failed job list could not be read".
    // Each section: the next poll's WakeAt is at poll_every (no backoff), and no "glab-auth" runs next poll.
}
TEST_CASE("an in_progress_jql runs once per poll and decides progress", "[orchestrator][engine][jira][work]")
{
    // jira source jql "assignee = currentUser()", in_progress_jql 'status = "Selected for Development"':
    // one acli-issues with jql `(assignee = currentUser()) AND (status = "Selected for Development")`;
    // answered with PRJ-1240 only → listedIssues: PRJ-1240 InProgress, PRJ-1234 (indeterminate) NotStarted.
    // SECTION "a failed query keeps the default rule": exit 1 → PRJ-1234 InProgress, problem line names the query;
    // the next poll's WakeAt is at poll_every (no backoff).
    // SECTION "a progress-only change shows": a later poll answers the query with PRJ-1234 too, nothing else
    // changes → revision() rises and listedIssues marks PRJ-1234 InProgress.
}
TEST_CASE("one key lookup per poll for tickets MRs name that no source listed", "[orchestrator][engine][jira][work]")
{
    // GitHub repo with PRs titled "PRJ-1999: …", "UTF-8 fix", "OTHER-5 …" and a Jira source listing PRJ-1234:
    // after the lists, exactly one acli-issues whose jql is "key in (PRJ-1999)" (no UTF-8, no OTHER-5:
    // Review Focus 2); answered with PRJ-1999 → linkedTickets()[0].item == JiraRef{"PRJ-1999"};
    // the poll's backoff starts only after it answered; revision() rises when linkedTickets() changes.
    // A poll where every key was listed issues no lookup. A failed lookup keeps the previous linkedTickets(), adds a
    // problem line, and does not back the poll off.
}
TEST_CASE("workItems joins the engine's lists in configuration order", "[orchestrator][engine][work]")
{
    // two repos listing the same Jira ticket → one item, of the first repo.
}
```

- [ ] **Step 2:** Build; `orchestrator_test.exe "[work]"` → the engine cases fail.
- [ ] **Step 3: Implement.** Keep `PollTagPrefix` parsing table-driven: the list names are rows the parser looks
  up, not a chain of `if`s.
- [ ] **Step 4:** Rebuild; `orchestrator_test.exe` (all, including the existing `[gitlab]` and `[jira]` engine
  cases, whose expected command counts grow by the new commands only where configured) → pass; portability and
  include budget → 0; `clang-format -i`.
- [ ] **Step 5: Commit** — message `feat(orchestrator): poll GitLab pipelines, in-progress queries and linked tickets`.

### Task 8: A dispatch may name its worker — Haiku

**Files:**
- Modify: `extensions/orchestrator/core/Engine.hpp` (`Dispatch::worker`, two `DispatchRefusal` values),
  `extensions/orchestrator/core/EngineDispatch.cpp` (`preview`)
- Modify: `extensions/orchestrator/core/Config.hpp/.cpp` (`takesNoProfileFor`, which the anonymous
  `profileRefused` (~Config.cpp:1210) now calls)
- Modify: `extensions/orchestrator/view/DispatchForm.hpp/.cpp` (`DispatchAction::worker`, `refusalText` rows),
  `extensions/orchestrator/guest/Driver.cpp` (passes `worker` from `DispatchAction` to `Dispatch`)
- Test: `extensions/orchestrator/core/Engine_test.cpp`, `extensions/orchestrator/core/Config_test.cpp`,
  `extensions/orchestrator/view/DispatchForm_test.cpp`, `extensions/orchestrator/guest/Driver_test.cpp`

**Interfaces:**
- Consumes: `Config::workerFor(taskId)`, `Config::findWorker(id)`, `agentRowOf(AgentKind).settings`
  (`SettingsProfile::{NotTaken,Taken}`, `core/Agents.hpp`), `TaskDef::permissions`.
- Produces:

```cpp
// Config.hpp
/// @return Whether @p task has its own permission profile (`permissions`) and @p worker's agent takes none: the
///         configuration's ProfileOnCopilot rule and a dispatch's WorkerTakesNoProfile share it.
[[nodiscard]] bool takesNoProfileFor(TaskDef const& task, WorkerDef const& worker) noexcept;

// Dispatch and DispatchAction each gain, after `permissions`:
std::string worker = {}; ///< The worker it runs on, this run only; empty: the task's (Config::workerFor).
// DispatchRefusal gains, last:
UnknownWorker,        ///< No worker has that id.
WorkerTakesNoProfile, ///< The worker's agent takes no permission profile, and the task has its own.
// refusalText rows: "no worker has that id: check `workers`";
//                   "that worker takes no permission profile, and this task has one: pick another worker"
```

  `preview`: `auto const* worker = request.worker.empty() ? &_config->workerFor(task->id) :
  _config->findWorker(request.worker)`; null → `UnknownWorker`; `takesNoProfileFor(*task, *worker)` →
  `WorkerTakesNoProfile`. `request.permissions` does not count: the dialog always sends a profile name, and a
  no-profile worker runs without it, as today. The stored `run.worker` stays empty for the implicit worker alone
  (as today).

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST_CASE("a dispatch's worker overrides the task's for that run only", "[orchestrator][engine][workers]")
{
    // workers: claude-a (env_set a), claude-b (env_set b); task fix_ci worker claude-a.
    // Dispatch{ worker = "claude-b" } → run.worker == "claude-b", run.envSet == "b", the OpenPane's arguments hold
    // {EnvSetArgument, "b"}; a second dispatch without worker on another PR → "claude-a".
    // preview(Dispatch{ worker = "nobody" }) == std::unexpected(DispatchRefusal::UnknownWorker).
}
TEST_CASE("a worker that takes no profile is refused for a task with one", "[orchestrator][engine][workers]")
{
    // workers: claude, copilot (agent copilot); task fix_ci (worker claude) with permissions "read_only".
    // Dispatch{ worker = "copilot" } → WorkerTakesNoProfile (Review Focus 3); a task without permissions, dispatched
    // with worker "copilot" and permissions "default" (as the dialog sends) → accepted, run.agent == AgentKind::Copilot,
    // run.permissions empty.
}
```

  `Config_test.cpp` (add to Test): the existing Copilot-profile cases still pass through `takesNoProfileFor`, and
  `takesNoProfileFor({ .permissions = "read_only" }, { .agent = AgentKind::Copilot })` is true,
  `… { .agent = AgentKind::Claude }` false, `{ .permissions = "" }` with Copilot false.
  `DispatchForm_test.cpp`: `refusalText(DispatchRefusal::UnknownWorker)` and `WorkerTakesNoProfile` are non-empty
  and distinct. `Driver_test.cpp`: a `DispatchAction` with `worker = "claude-b"` steps a `Dispatch` with the same.
- [ ] **Step 2–4:** build → fail; implement; rebuild → `orchestrator_test.exe` (all) pass; portability;
  `clang-format -i`.
- [ ] **Step 5: Commit** — message `feat(orchestrator): a dispatch may run on another worker than its task's`.

### Task 9: The Work view — board, keys, folds, sort, filter, switching with `w` — Opus

**Files:**
- Create: `extensions/orchestrator/view/WorkBoard.hpp`, `extensions/orchestrator/view/WorkBoard.cpp`,
  `extensions/orchestrator/view/WorkBoard_test.cpp`
- Modify: `extensions/orchestrator/view/Keys.hpp/.cpp` (`View`, `WorkState` in `Interaction`, new `KeyCommand`s,
  `workKeyRows()`), `extensions/orchestrator/view/Dashboard.cpp` (top bar view names, the Work layout, frame bytes),
  `extensions/orchestrator/view/Actions.cpp` (palette rows `SwitchView`, action-bar hints in the Work view)
- Modify: `extensions/orchestrator/view/ViewFixtures.hpp/.cpp` (`World::answers`)
- Create (hand-made): `extensions/orchestrator/core/testdata/github/prs-work.json` (projected rows; a row in
  `core/testdata/github/README.md`), `extensions/orchestrator/core/testdata/jira/issues-lookup-work.json` (the key
  lookup's answer; a row in `core/testdata/jira/README.md`)
- Create (goldens): `view/testdata/work-{50x20,80x24,120x40,160x40}.screen`, `work-folded-{80x24,120x40}.screen`,
  `work-flat-{80x24,120x40}.screen`, `work-filter-{80x24,120x40}.screen`
- Modify: `extensions/orchestrator/CMakeLists.txt`
- Test: `extensions/orchestrator/view/WorkBoard_test.cpp`, `extensions/orchestrator/view/Dashboard_test.cpp` (scenes)

**Interfaces:**
- Consumes: `Engine::workItems() const -> std::vector<WorkItem>` and `Engine::revision()` (Task 7: it rises when
  the lists, issue progress or the looked-up tickets change); from `core/WorkItems.hpp` (Task 5): `WorkItem`
  (`group`, `repo`, `changeRequest`, `ticket`, `ticketKey`, `item`, `updatedWallMs`, `agentRun`, `agentWorker`),
  `WorkGroup`, `WorkGroupRows` (`foldKey`, `name`, `FoldDefault`), `SortByUpdateKey` (`"work/sort-by-update"`),
  `WorkSort`, `orderOf(items, repoOrder, sort)`, `matchesFilter(item, filter)`, `workItemKey(item)`;
  `ToggleFoldAction`, `Engine::isFolded(group)`, `OpenUrlAction`, `jira::browseUrl`.
- Produces:

```cpp
// Keys.hpp
/// Which view the dashboard shows.
enum class View : uint8_t { Agents = 0, Work };
// KeyCommand gains, before Reply1: SwitchView (`w`), Sort (`s`), RepoPrevious, RepoNext (Work view's ←/→).
/// Where the Work view's keys go.
enum class WorkFocus : uint8_t { List = 0, Filter, RepoChip, SortChip };
/// The Work view's interaction (the filter does not persist; folds and sort are fold keys).
struct WorkState
{
    std::size_t selected = 0;              ///< Into workLines(): headings and items.
    std::size_t scroll = 0;                ///< The first line shown.
    WorkFocus focus = WorkFocus::List;
    Filtering filtering = Filtering::Off;
    std::string filter = {};
    std::size_t repoChip = 0;              ///< 0: every repository; else 1 + its index in the configuration.
    std::string selectedKey = {};          ///< workItemKey() of the selected item, or `group:<foldKey>` of a heading.
    bool operator==(WorkState const&) const = default;
};
// Interaction gains: View view = View::Agents; WorkState work = {};
/// @return The keys the Work view acts on (none is added to dashboardKeys(): that would
///         refuse today's configurations with a task keyed `w`, `s` or `o`).
[[nodiscard]] std::span<KeyRow const> workKeyRows() noexcept;

// WorkBoard.hpp
enum class WorkLineKind : uint8_t { Heading = 0, Item };
struct WorkLine { WorkLineKind kind; WorkGroup group; std::size_t item = 0; bool operator==(WorkLine const&) const = default; };
/// @return Whether @p group is folded: its default, flipped when the engine holds its fold key.
[[nodiscard]] bool isGroupFolded(Engine const& engine, WorkGroup group) noexcept;
/// @return The sort the engine's fold keys hold.
[[nodiscard]] WorkSort sortOf(Engine const& engine) noexcept;
/// @return The lines drawn: Importance → per non-empty group a heading (`Needs you  3`) then its items unless
///         folded; LastUpdate → items only. Items the filter or the repository chip hide are left out, and a
///         group left empty by them loses its heading.
[[nodiscard]] std::vector<WorkLine> workLines(std::span<WorkItem const> items, std::span<std::size_t const> order,
                                              Engine const& engine, WorkState const& state);
/// @return `45s`, `12m`, `3h`, `2d`: the age of a last update.
[[nodiscard]] std::string updateAgeText(uint64_t ms);
/// Applies one Work-view command (KeyCommand); Fold on a heading or `z` folds by ToggleFoldAction of the group's
/// key; Sort toggles SortByUpdateKey; Enter on a heading folds; Enter on an item is left to the caller (Task 10).
[[nodiscard]] std::optional<UiAction> applyWorkCommand(KeyCommand command, Interaction& interaction,
                                                       std::span<WorkLine const> lines, std::span<WorkItem const> items,
                                                       Engine const& engine, std::size_t pageRows);
/// The Work view: the filter bar (filter, repository chip, sort chip), the lines (only those visible), the
/// scrollbar on the list's right edge, the details line. Records its hits (Task 12 adds the kinds).
class WorkBoardView final: public core::tui::Component { /* ctor(Engine const&, Interaction const&, HitMap&) */ };
```

  Work key rows: `up`/`k`, `down`/`j`, `pgup`, `pgdn`, `home`, `end`, `enter`, `z` Fold, `/` Filter, `esc` Back
  (clears the filter), `s` Sort, `left` RepoPrevious, `right` RepoNext, `o` OpenLink, `n` NewAgent (Task 11 renames),
  `w` SwitchView, `^k`/`?` Palette, `tab` (moves `WorkFocus`). In the Agents view, `w` is SwitchView too.
  A row's columns (spec, **A row**): MR `!812` (GitLab) / `#2375` (GitHub); Ticket key; Title (`…`-cut);
  Pipeline `✗ pipeline` (Failing) / `◌ running` (Pending) / `✓ pipeline` (Passing) / `·`; Review `⚠ conflicts`
  (wins) / `⟲ changes` / `✓ approved` / `… review` (Required) / `· no review` (None); Jira `◆ <status>` (a forge
  issue `● open`, a key without a status `◆ ?`, none `·`); Agent `⬢ <worker>` or `updateAgeText`. Roles:
  Failing/Warning/Passing/Working/Dim as the Fleet pairs glyph and colour. Narrow widths drop, in order: the Agent
  column's age (a `⬢ worker` stays), then Review text → glyph, then Pipeline text → glyph, then the Ticket column
  under 60 columns. The MR and ticket cells are drawn **without** OSC 8 hyperlinks: `Dashboard::applyMouse` opens
  any hyperlink on a single press, and the spec wants a click to select (double-click opens, Task 12). The top
  bar shows `Agents  Work` (the shown one in the Focus role) before the gauge and pill; the Work view replaces the
  Fleet and Focus panes. While it is shown the frame also sets `\033[?1003h` and on leaving `\033[?1003l`
  (any-motion reporting, for Task 12's hover and drag; without motion events nothing else changes).
  `World::answers`: `std::vector<CommandAnswer>` where `struct CommandAnswer { std::string command; std::string
  argumentContains = {}; ListAnswer answer; }`, consulted first by `answerFor` (first match whose command equals and
  some argument value contains `argumentContains`).
  `prs-work.json` (hand-made, projected `gh-prs` rows, `updatedAt` around `2026-11-13T12:00:00Z`): #2375 "PRJ-1234:
  fix the flaky test" Failing (`test-windows`); #2376 "address the review" CHANGES_REQUESTED; #2377 "PRJ-1301:
  rebase" CONFLICTING + APPROVED; #2378 "ready one" passing + APPROVED; #2379 "PRJ-1999: running" pending; #2380
  "draft work" isDraft. The Work world: `OneRepository` plus a Jira source (`site: https://example.atlassian.net`),
  `gh-prs` → `prs-work.json`, `acli-issues` → `jira/issues-work.json` (Task 3: PRJ-1234, PRJ-1240, PRJ-1300,
  PRJ-1301, PRJ-1302 `indeterminate` and unnamed), the key lookup (`argumentContains = "key in"`) →
  `jira/issues-lookup-work.json`: one work item, `PRJ-1999` "In Review"/`indeterminate`, acli's shape, site
  `example.atlassian.net`.

- [ ] **Step 1: Write the failing tests** (`WorkBoard_test.cpp`, `[orchestrator][view][work]`):

```cpp
TEST_CASE("Work view: w switches views and back", "[orchestrator][view][work]")
{
    // fixture.dashboard.input("w") → interaction().view == View::Work; input("w") → Agents; Tab in Agents still
    // moves Fleet/Focus; in Work, Tab moves WorkFocus List → Filter → RepoChip → SortChip → List.
}
TEST_CASE("Work view: groups in order, Everything else folded", "[orchestrator][view][work]")
{
    // workLines of the Work world: headings in the order Needs you, Ready to merge, In review, No MR yet,
    // Everything else; Everything else has no item lines; Needs you holds #2375, #2376, #2377 newest first;
    // No MR yet holds PRJ-1302; #2379's Jira cell reads "◆ In Review" (from the lookup).
}
TEST_CASE("Work view: z folds the selected row's group and the fold persists", "[orchestrator][view][work]")
{
    // select an item of Needs you, input("z") → actions == { ToggleFoldAction{ "work/needs-you" } };
    // after carryOut: isGroupFolded(engine, NeedsYou); the selection is on the heading.
    // Enter on the Everything else heading → ToggleFoldAction{ "work/else" } and it unfolds.
}
TEST_CASE("Work view: s switches to one flat list by last update, persisted", "[orchestrator][view][work]")
{
    // input("s") → ToggleFoldAction{ "work/sort-by-update" }; after carryOut: no Heading lines, items newest first.
}
TEST_CASE("Work view: the filter, the repository chip and Esc", "[orchestrator][view][work]")
{
    // "/2375" → only #2375 listed; Esc → filter empty, everything back; "/prj-1301" → #2377;
    // two repositories: → steps the chip to the first repository, ← back to all.
}
TEST_CASE("Work view: the selection stays in view while it moves and the list scrolls", "[orchestrator][view][work]")
{
    // a world of 60 items at 80x24: End → selected is the last line and scroll shows it; PgUp moves by a page.
}
TEST_CASE("Work view: a long list draws only its visible lines", "[orchestrator][view][work]")
{
    // 100 PRs (github/prs-100.json) at 120x40: rendering completes and rowBuilds() does not grow per frame without
    // an engine change (the items are rebuilt only when engine.revision() changes).
}
TEST_CASE("Work view: o opens the selected MR or ticket", "[orchestrator][view][work]")
{
    // on #2375 → OpenUrlAction{ url = its URL, label = "#2375" }; on a ticket row → its browse URL, label its key.
}
```

  `Dashboard_test.cpp` scenes: `work` (keys `w`) at Narrow, Medium, Wide and `{160, 40}`; `work-folded` (keys
  `wz`, selecting Needs you's first item) at Medium and Wide; `work-flat` (keys `ws`) and `work-filter` (keys
  `w/prj`) at Medium and Wide. The scenes' world is the Work world.
- [ ] **Step 2:** Add files to `CMakeLists.txt`; build; `orchestrator_test.exe "[work]"` → fail.
- [ ] **Step 3: Implement.** The items are cached in the dashboard's state and rebuilt only when
  `engine.revision()` changes; the view renders only the lines that fit (Constraints: the daemon's call deadline).
  The selection is kept by `selectedKey` across rebuilds; when its line is gone it moves to the nearest line.
- [ ] **Step 4: Goldens.** `ORCHESTRATOR_UPDATE_GOLDENS=1 … "golden screens"`; read every new `work-*.screen`:
  each column the spec names is present at 120 and 160 columns, the narrow-width drops at 80 and 50; the
  existing screens are unchanged except the top bar's `Agents  Work` names (read that diff in full).
- [ ] **Step 5:** `orchestrator_test.exe` (all); `ctest --test-dir out/build/clangcl-release -R
  "orchestrator|fixture-privacy"` → pass; portability → 0; `clang-format -i`.
- [ ] **Step 6: Commit** — message `feat(orchestrator): the Work view — grouped, folding, sortable and filtered`.

### Task 10: The start dialog — Opus

**Files:**
- Create: `extensions/orchestrator/view/StartDialog.hpp`, `extensions/orchestrator/view/StartDialog.cpp`,
  `extensions/orchestrator/view/StartDialog_test.cpp`
- Modify: `extensions/orchestrator/view/DispatchForm.hpp/.cpp` (`DispatchFormState::worker`, `FormField::Worker`,
  `workerOf`, `actionOf`, `changeSetting`, `openDispatchForm`, `openTopicForm`)
- Modify: `extensions/orchestrator/view/Keys.hpp` (`Interaction` gains `std::optional<StartDialogState>
  startDialog`: in `Interaction`, not `WorkState`, because a finished run's "run again" opens it from the Agents
  view), `extensions/orchestrator/view/Dashboard.cpp` (shows the dialog as an overlay, as the launcher was; `⏎` on
  an item opens it; while open it takes every key)
- Create (goldens): `view/testdata/start-dialog-{50x20,80x24,120x40}.screen`
- Modify: `extensions/orchestrator/CMakeLists.txt`
- Test: `StartDialog_test.cpp`, `DispatchForm_test.cpp`, `Dashboard_test.cpp` (scenes)

**Interfaces:**
- Consumes: from `core/WorkItems.hpp` (Tasks 5–6): `WorkItem`, `suggestTask(Config const&, WorkItem const&) ->
  TaskSuggestion { TaskDef const* task; std::string reason; }`, `defaultWorker(Config const&, std::span<Run const>,
  std::string_view taskId) -> WorkerDef const&`, `activeRunsOn(Config const&, std::span<Run const>,
  std::string_view workerId) -> std::size_t`; `DispatchAction::worker` and `DispatchRefusal::{UnknownWorker,
  WorkerTakesNoProfile}` with their `refusalText` rows, `takesNoProfileFor(TaskDef const&, WorkerDef const&)` (Task 8);
  `Engine::workItems()`, `Engine::runs()`, `Engine::activeRunOn(repo, item)`; existing `DispatchForm.hpp`:
  `isSetupIncomplete`, `setupWarnings`, `previewOf`, `canConfirm`, `commandShown`, `checkoutText`, `refusalText`,
  `HooksWarning`, `FormField::{Task,Model,Note,Repo,Permissions}`.
- Produces:

```cpp
// DispatchForm.hpp
// DispatchFormState gains: std::size_t worker = 0; ///< Index into Config::workers().
// FormField gains, last: Worker ///< ←/→ cycle the workers.
// workerOf(form, engine) now returns &workers()[form.worker]; actionOf sets DispatchAction::worker to its id;
// changeSetting(Task) also resets worker to defaultWorker(task). openDispatchForm() and openTopicForm() set worker
// to the index of defaultWorker(engine.config(), engine.runs(), task) — never left at 0 (pre-flight finding 2).
// FormField::Permissions stays: the profile is the dialog's fifth setting, defaulting as today.

// StartDialog.hpp
/// How much of what a dispatch runs the dialog shows.
enum class DialogPreview : uint8_t { Short = 0, Full };
/// An open start dialog.
struct StartDialogState
{
    DispatchFormState form;                    ///< What ⏎ dispatches.
    std::string reason;                        ///< Why its task was suggested (TaskSuggestion::reason).
    FormField setting = FormField::Task;       ///< What ←/→ change.
    HooksWarning hooks = HooksWarning::None;   ///< As the launcher had it: a second ⏎ past an incomplete setup.
    DialogPreview preview = DialogPreview::Short;
    bool operator==(StartDialogState const&) const = default;
};
/// Why the dialog did not open.
enum class StartRefusal : uint8_t { NotConfigured = 0, NotListed, NoTask };
/// @return The dialog on @p item: the suggested task (or @p task while it applies, for a finished run's "run
///         again"), its model, defaultWorker(), and the reason; or why it cannot open.
[[nodiscard]] std::expected<StartDialogState, StartRefusal> openStartDialog(Engine const& engine, WorkItem const& item,
                                                                            std::string_view task, std::string session);
/// Applies one key: Tab moves Task → Worker → Model → Perms → Note → Task; ←/→ step the setting; typing edits the note;
/// Ctrl+P toggles the preview; Esc closes; ⏎ dispatches unless canConfirm() is false (the engine's refusal is shown)
/// or the worker's setup is incomplete and hooks != Confirmed (first ⏎ only confirms).
[[nodiscard]] std::optional<DispatchAction> applyStartDialogKey(core::tui::KeyEvent const& key,
                                                                std::optional<StartDialogState>& dialog,
                                                                Engine const& engine, HooksWarning hooksAtInput,
                                                                std::string_view nearTab);
/// The dialog, in a rounded box titled `▶ Start agent`: the item, Task (with `‹ ›` and the reason), Worker (each
/// worker as a chip `name n` with its active runs, `(setup)` when isSetupIncomplete, `(no profile)` when
/// takesNoProfileFor(task, worker)), Model, Perms, Note, the checkout and worktree, the setup warnings,
/// `⬢ <worker> already works on this` when an active run is on the item (⏎ then shows the engine's AlreadyRunning
/// refusal; there is no second-⏎ override), and with Full the prompt and command line.
class StartDialogView final: public core::tui::Component { /* ctor(Engine const&, std::optional<StartDialogState> const&, HitMap&) */ };
```

- [ ] **Step 1: Write the failing tests** (`StartDialog_test.cpp`, `[orchestrator][view][start]`):

```cpp
TEST_CASE("Start dialog: ⏎ on #2375 suggests fix CI with its failing check", "[orchestrator][view][start]")
{
    // Work world, select #2375, Enter → the dialog is open; form.task names fix_ci; reason == "pipeline fails: test-windows".
}
TEST_CASE("Start dialog: the worker defaults by the busy rule and can be changed", "[orchestrator][view][start]")
{
    // workers a (task's), b; one active run on a, on another PR (#2378) → the dialog on #2375 opens on b.
    // Tab (to Worker), → (one step, wraps to a); Enter → DispatchAction{ .worker = "a", .task = "fix_ci",
    // .item = ChangeRequestRef{2375} }.
}
TEST_CASE("Start dialog: the permission profile is its fifth setting", "[orchestrator][view][start]")
{
    // Tab ×3 reaches Perms; it starts on the task's profile (else the default); → steps it; Enter → the
    // DispatchAction's permissions is the stepped profile.
}
TEST_CASE("Start dialog: Copilot chosen for a task without permissions dispatches", "[orchestrator][view][start]")
{
    // workers claude, copilot; task fix_ci without `permissions`: choose copilot, Enter → one DispatchAction with
    // worker "copilot" (no WorkerTakesNoProfile refusal shown). A task with `permissions`: the chip reads
    // "copilot (no profile)" and Enter shows refusalText(WorkerTakesNoProfile).
}
TEST_CASE("Start dialog: an incomplete setup asks for a second ⏎", "[orchestrator][view][start]")
{
    // hooks missing (contour/setup-hooks-missing.json): first Enter → no action, hooks == Shown; second → dispatch.
}
TEST_CASE("Start dialog: an item an agent works on shows so and does not dispatch", "[orchestrator][view][start]")
{
    // active run on #2375 by worker "claude": the dialog shows "⬢ claude already works on this"; Enter → no action,
    // the dialog shows refusalText(AlreadyRunning).
}
TEST_CASE("Start dialog: the list changing under it dispatches nothing stale", "[orchestrator][view][start]")
{
    // open on #2375; the next poll lists no #2375 (engineChanged) → Enter dispatches nothing and the dialog shows
    // refusalText(BaseUnknown) (Review Focus 5).
}
```

  `DispatchForm_test.cpp`: `changeSetting(form, FormField::Worker, 1, engine)` cycles the workers and wraps;
  `actionOf` carries `worker`; `openDispatchForm` and `openTopicForm` open on the task's worker (workers a, b, the
  task's b, no runs → `form.worker == 1`), and on the least busy one when the task's has an active run. Scenes: `start-dialog` (keys `w` then `\r` on Needs you's first item) at Narrow,
  Medium, Wide.
- [ ] **Step 2:** build → fail. **Step 3: Implement.** **Step 4:** goldens (read them: the reason, the worker chips
  with their counts, the checkout line). **Step 5:** `orchestrator_test.exe` (all); `ctest … -R orchestrator` →
  pass; portability; `clang-format -i`.
- [ ] **Step 6: Commit** — message `feat(orchestrator): the start dialog, with a suggested task and a choosable worker`.

### Task 11: `n` starts a custom task — Opus

**Files:**
- Create: `extensions/orchestrator/view/TaskBox.hpp`, `extensions/orchestrator/view/TaskBox.cpp`,
  `extensions/orchestrator/view/TaskBox_test.cpp`
- Modify: `extensions/orchestrator/view/Keys.hpp/.cpp` (`NewAgent` → `NewTask`; `RunAgain` on a topic run opens
  the box with its text, on an item run the start dialog), `extensions/orchestrator/view/Actions.cpp` (label
  `new task`), `extensions/orchestrator/view/Sections.cpp/.hpp` (`RowKind::NewAgent` → `NewTask`, `+ new task`),
  `extensions/orchestrator/view/Dashboard.hpp/.cpp` (`openLauncherOn` → `openStartDialogOn`; the box overlay)
- Create (goldens): `view/testdata/task-box-{50x20,80x24,120x40}.screen`; regenerated Fleet goldens (`+ new task`)
- Modify: `extensions/orchestrator/CMakeLists.txt` (TaskBox files)
- Modify/Delete (the launcher's tests that reach it through `n` or `openLauncherOn`, so this commit is green; the
  launcher's code itself goes in Task 13): `view/DispatchForm_test.cpp` ("dispatch form goldens", "the launcher
  browses…", "…names each row's repository…", "topic form goldens", "a finished topic run's form…"),
  `view/Launcher_test.cpp` (every case that opens the launcher through `n`, `openLauncherOn` or `RunAgain`),
  `view/Dashboard_test.cpp` (the launcher scenes and "n opens the launcher…", "the launcher opens again as it was
  narrowed", "a click on a launcher candidate…"), `guest/Driver_test.cpp` (launcher references), and the
  `view/testdata/launcher-*.screen` goldens (13 files, `git rm` by name). Each deleted case is either covered by a
  Work-view, start-dialog or box test (name it in the commit body) or rewritten onto them.
- Test: `TaskBox_test.cpp`, `Keys_test.cpp`, `Dashboard_test.cpp`

**Interfaces:**
- Consumes: `openTopicForm(Engine const&, std::string_view repo, std::string topic, std::string session) ->
  std::optional<DispatchFormState>` (it now sets `worker` to `defaultWorker`'s index, Task 10); `defaultWorker(Config
  const&, std::span<Run const>, std::string_view taskId) -> WorkerDef const&` (Task 6); `StartDialogState`,
  `StartRefusal::{NotConfigured,NotListed,NoTask}`, `openStartDialog(Engine const&, WorkItem const&,
  std::string_view task, std::string session) -> std::expected<StartDialogState, StartRefusal>` and
  `Interaction::startDialog` (Task 10); `workItemKey`/`Engine::workItems()` (Tasks 5, 7); `HooksWarning`,
  `isSetupIncomplete`, `actionOf`, `canConfirm` (`DispatchForm.hpp`); `Config::firstTaskFor(AppliesTo::Topic)`.
- Produces:

```cpp
/// Where the `n` box's keys go.
enum class TaskBoxField : uint8_t { Text = 0, Repo, Worker, Model };
/// An open `n` box.
struct TaskBoxState
{
    std::string text = {};                    ///< "What should the agent do?"
    TaskBoxField focus = TaskBoxField::Text;
    DispatchFormState form;                   ///< A topic form: repository, worker, model.
    HooksWarning hooks = HooksWarning::None;
    bool operator==(TaskBoxState const&) const = default;
};
/// @return The box: repository = the repository chip's, else the selected row's, else the first; worker =
///         defaultWorker(topic task); model = the topic task's; @p text typed (a finished topic run's, or empty).
///         StartRefusal::NotConfigured when the engine has no configuration or no repository.
[[nodiscard]] std::expected<TaskBoxState, StartRefusal> openTaskBox(Engine const& engine, std::string_view chipRepo,
                                                                    std::string_view selectedRepo, std::string text,
                                                                    std::string session);
/// Typing edits the text; Tab moves Text → Repo → Worker → Model → Text; ←/→ step a setting; ⏎ dispatches the
/// text as a topic at once (nothing to choose), not when the text is blank; Esc closes. Incomplete setup: second ⏎.
[[nodiscard]] std::optional<DispatchAction> applyTaskBoxKey(core::tui::KeyEvent const& key, std::optional<TaskBoxState>& box,
                                                            Engine const& engine, HooksWarning hooksAtInput, std::string_view nearTab);
class TaskBoxView final: public core::tui::Component { /* ctor(Engine const&, std::optional<TaskBoxState> const&, HitMap&) */ };
// Interaction gains: std::optional<TaskBoxState> taskBox; (opened from either view).
// Dashboard: void openStartDialogOn(std::string_view repo, ItemRef const& item, std::string_view task); replaces
// openLauncherOn: it finds the WorkItem of @p repo and @p item in engine.workItems() (none → the action bar says
// StartRefusal::NotListed's text) and opens openStartDialog() on it. A topic run's RunAgain opens the box with
// Run::topic and its repository; an item run's opens the start dialog on its item and task.
```

- [ ] **Step 1: Write the failing tests** (`[orchestrator][view][taskbox]`):

```cpp
TEST_CASE("n opens the box; ⏎ starts the text at once", "[orchestrator][view][taskbox]")
{
    // input("n"), type "investigate the flaky test", "\r" → one DispatchAction with item TopicRef, topic == that
    // text, repo == "contour", task == the topic task, worker == defaultWorker(topic task).
}
TEST_CASE("the n box: a blank text does not start", "[orchestrator][view][taskbox]")
{
    // "n", "   ", "\r" → no action; the box stays open.
}
TEST_CASE("the n box takes the repository chip, else the selected row's", "[orchestrator][view][taskbox]")
{
    // two repositories; Work view with the chip on "other" → box repo "other"; chip all, row of "other" selected →
    // "other"; Agents view, nothing selected → the first.
}
TEST_CASE("run again on a finished topic run opens the box with its text", "[orchestrator][view][taskbox]")
{
    // finished TopicRef run with topic "tidy the docs" in repo other → box.text == "tidy the docs", repo "other".
}
TEST_CASE("+ new task in the Fleet opens the box", "[orchestrator][view][taskbox]")
{
    // Enter on the `+ new task` row → the box is open.
}
```

- [ ] **Step 2–3:** build → fail; implement; delete or rewrite the launcher tests and goldens listed under Files.
  **Step 4:** goldens — the `task-box` scenes (keys `n` then text) at Narrow, Medium, Wide; regenerated Fleet screens
  may change only `+ new agent` → `+ new task` and the hint label (read every diff); `git status --short
  extensions/orchestrator/view/testdata` shows the 13 `launcher-*` deletions and nothing else unexpected. **Step 5:** `orchestrator_test.exe` (all) → pass; portability; `clang-format -i`.
- [ ] **Step 6: Commit** — message `feat(orchestrator): n starts a custom task directly`.

### Task 12: Mouse — hit regions, the row menu, the scrollbar, hover and copy — Opus

**Files:**
- Create: `extensions/orchestrator/view/RowMenu.hpp`, `extensions/orchestrator/view/RowMenu.cpp`,
  `extensions/orchestrator/view/RowMenu_test.cpp`
- Modify: `extensions/orchestrator/view/HitMap.hpp` (kinds), `extensions/orchestrator/view/Dashboard.cpp`
  (`MouseRows`, `applyMouse`: right button, motion), `WorkBoard.cpp`, `StartDialog.cpp`, `TaskBox.cpp` (record hits),
  `extensions/orchestrator/view/Keys.hpp/.cpp` (`WorkState` gains `menu`, `hovered`; `KeyCommand::RowMenu` and its
  `m` row in `workKeyRows()`), `extensions/orchestrator/CMakeLists.txt` (RowMenu files)
- Create (goldens): `view/testdata/row-menu-{80x24,120x40}.screen`
- Test: `RowMenu_test.cpp`, `HitMap_test.cpp`, `Dashboard_test.cpp`

**Interfaces:**
- Consumes: `WorkItem`, `workItemKey(WorkItem const&)` (Task 5); `WorkState`, `View`, `WorkFocus`, `WorkLine`,
  `workLines()`, `isGroupFolded()`, `WorkBoardView` (Task 9: MR/ticket cells carry no OSC 8 link);
  `StartDialogState`, `openStartDialog()`, `FormField::{Task,Worker,Model,Permissions,Note}`,
  `applyStartDialogKey()` (Task 10); `TaskBoxState`, `TaskBoxField` (Task 11); `ToggleFoldAction`, `OpenUrlAction`;
  `core::base64::encode(std::string_view)` (`<core/Base64.hpp>`); `core::tui::MouseEvent` (`Type::Move` with
  button 0 = drag, 3 = no button held); the dashboard's `Clicks`, `DoubleClickMs`, `WheelLines` (3), `MouseRows`.
- Produces:

```cpp
// HitKind gains, after LauncherKind (MouseRows stays indexed by HitKind; Task 13 removes the launcher kinds):
ViewName,      ///< A view's name in the top bar; value: View.
WorkRow,       ///< A Work line; value: its index into workLines().
WorkMrLink,    ///< An item's MR number cell; value: its line.
WorkTicketLink,///< An item's ticket key cell; value: its line.
WorkHeading,   ///< A group heading; value: WorkGroup.
WorkScrollbar, ///< The list's right edge; value: unused (the y decides).
WorkChip,      ///< The filter bar; value: WorkFocus (Filter, RepoChip, SortChip).
DialogSetting, ///< A dialog's or the n box's setting label; value: FormField / TaskBoxField.
DialogStep,    ///< Its value or `‹ ›`; value: 2 × field + (0 back, 1 forward).
WorkerChip,    ///< A worker chip; value: its index in Config::workers().
MenuEntry,     ///< A row menu entry; value: its index.

// RowMenu.hpp
enum class RowMenuCommand : uint8_t { StartAgent = 0, OpenMr, OpenTicket, CopyBranch, FoldGroup };
struct RowMenuEntry { RowMenuCommand command; std::string_view label; };
inline constexpr auto RowMenuEntries = std::array {
    RowMenuEntry { RowMenuCommand::StartAgent, "Start agent…" }, RowMenuEntry { RowMenuCommand::OpenMr, "Open MR" },
    RowMenuEntry { RowMenuCommand::OpenTicket, "Open ticket" }, RowMenuEntry { RowMenuCommand::CopyBranch, "Copy branch name" },
    RowMenuEntry { RowMenuCommand::FoldGroup, "Fold group" } };
struct RowMenuState { std::string itemKey; std::size_t highlighted = 0; core::tui::Point at {}; bool operator==(RowMenuState const&) const = default; };
/// @return Whether @p command applies to @p item: OpenMr and CopyBranch need an MR, OpenTicket a ticket URL.
[[nodiscard]] bool menuApplies(RowMenuCommand command, WorkItem const& item) noexcept;
/// @return The OSC 52 bytes that put @p text on the clipboard: `\033]52;c;<base64>\a` (core::base64::encode).
[[nodiscard]] std::string clipboardWrite(std::string_view text);
```

  `MouseRow` gains `RightClickHandler rightClick = nullptr;` (the `static_assert` that `MouseRows` ends at
  `LauncherKind` moves to `MenuEntry`); the spec's **Mouse** table is the rows' content:
  Row — click selects, double-click opens the dialog, right-click opens the menu at the pointer, wheel scrolls 3
  lines with the selection kept until it leaves view; MR/ticket cell — click selects, double-click opens it in the
  browser (`OpenUrlAction`), right-click and wheel as the row; heading — click/double-click fold, right-click folds
  every other group, wheel scrolls; scrollbar — click jumps there, button-0 motion drags the thumb; chips — click
  focuses the filter / steps a chip, right-click steps back, wheel steps; `ViewName` — switches; dialog/box —
  click picks a setting, on `DialogStep` steps it, a worker chip picks it, double-click starts, right-click steps
  back, wheel steps the setting under the pointer; a click outside a dialog or menu closes it (the existing popup
  rule). Hover: a button-3 `Move` over a different `WorkRow` sets `WorkState::hovered` and repaints; one over the
  same row does nothing (no repaint). Keyboard parity: `KeyCommand` gains `RowMenu`, bound to `m` in
  `workKeyRows()` only (`m` is already among `dashboardKeys()`), which opens the menu on the selected item; the
  menu takes ↑/↓/⏎/Esc. `WorkState` gains `std::optional<RowMenuState> menu` and `std::optional<std::size_t>
  hovered`. A menu whose item is gone after a rebuild closes. While the Work view is shown each frame sets
  `[?1003h` (Task 9); without motion events everything else works and the scrollbar takes clicks.

- [ ] **Step 1: Write the failing tests** (`[orchestrator][view][mouse][work]`; reuse `Dashboard_test.cpp`'s
  `clickAt`/`wheelAt` helpers, add `rightClickAt` = `\x1b[<2;x;yM\x1b[<2;x;ym` and `moveAt(button)` =
  `\x1b[<{32+button};x;yM`):

```cpp
TEST_CASE("Work mouse: a right-click on a row opens its menu; Open MR opens it", "[orchestrator][view][mouse][work]")
{
    // right-click on #2375's row → menu open with 5 entries; click "Open MR" → OpenUrlAction{ #2375's URL }.
    // On a ticket row "Open MR" and "Copy branch name" are dimmed and do nothing.
}
TEST_CASE("Work mouse: copy branch name writes OSC 52", "[orchestrator][view][mouse][work]")
{
    // menu on #2375, "Copy branch name" → the next frame() contains clipboardWrite(<its head branch>) and the
    // action bar says "copied <branch>". clipboardWrite("ab") == "\x1b]52;c;YWI=\x07".
}
TEST_CASE("Work mouse: a click on a heading folds; a right-click folds the others", "[orchestrator][view][mouse][work]")
{
    // click Needs you's heading → ToggleFoldAction{ "work/needs-you" }; right-click it → ToggleFoldActions for the
    // other open groups (ready, in-review, no-mr), none for else (already folded).
}
TEST_CASE("Work mouse: the wheel scrolls the list and keeps the selection until it leaves view", "[orchestrator][view][mouse][work]")
{
    // 60 items at 80x24, selection on line 2: wheel down once → scroll == 3, selected becomes the first visible line.
}
TEST_CASE("Work mouse: the scrollbar jumps on a click and follows a drag", "[orchestrator][view][mouse][work]")
{
    // click at the bottom cell of the right edge → scroll at its end; press at its top then moveAt(0) halfway →
    // scroll about half.
}
TEST_CASE("Work mouse: hover repaints only when the row under the pointer changes", "[orchestrator][view][mouse][work]")
{
    // moveAt(3) over row 1 → frame() non-empty; again over row 1 → frame() empty; over row 2 → non-empty.
}
TEST_CASE("Work mouse: double-click on the ticket key opens the ticket", "[orchestrator][view][mouse][work]")
{
    // → OpenUrlAction{ "https://example.atlassian.net/browse/PRJ-1234", "PRJ-1234" }.
}
TEST_CASE("Work mouse: a single click on an MR or ticket cell only selects", "[orchestrator][view][mouse][work]")
{
    // one press on #2375's MR cell → no action, the row is selected; one press on its ticket cell → the same.
}
TEST_CASE("Work mouse: double-click on a row opens the start dialog", "[orchestrator][view][mouse][start]")
{
    // two presses on #2375's row (outside its MR and ticket cells) within DoubleClickMs → interaction().startDialog
    // is open on ChangeRequestRef{2375}.
}
TEST_CASE("Work mouse: the dialog's settings step under the pointer", "[orchestrator][view][mouse][start]")
{
    // dialog open: click a worker chip → form.worker is it; wheel over Model steps it; right-click on Task's value
    // steps back; a click outside closes the dialog.
}
TEST_CASE("Work mouse: a click on Agents/Work in the top bar switches views", "[orchestrator][view][mouse][work]")
{
    // click the `Work` name → interaction().view == View::Work; click `Agents` → View::Agents.
}
TEST_CASE("Row menu: a poll that drops its item closes it", "[orchestrator][view][work]")
{
    // menu on #2375; the next poll lists no #2375 → the menu is closed (Review Focus 5).
}
```

  `HitMap_test.cpp`: the Work board records one `WorkRow` per drawn item line and one `WorkScrollbar` region.
  Scenes: `row-menu` (keys `w`, then a right-click as bytes on the first item) at Medium and Wide.
- [ ] **Step 2–3:** build → fail; implement. **Step 4: Check the host.** Run the dashboard in Contour
  (`contour daemon` with the orchestrator configured, open its tab): with `?1003h` set, moving the pointer over
  the Work rows highlights them and dragging the thumb scrolls. Write what was observed into the task's report; if
  motion does not arrive, nothing else changes and the README (Task 14) says hover and drag need a
  terminal that reports motion. **Step 5:** goldens; `orchestrator_test.exe` (all) → pass; portability;
  `clang-format -i`.
- [ ] **Step 6: Commit** — message `feat(orchestrator): mouse for the Work view, its dialogs and a row menu`.

### Task 13: Remove the launcher — Opus

**Files:**
- Delete: `extensions/orchestrator/view/Launcher.hpp`, `Launcher.cpp`, `Launcher_test.cpp` (what Task 11 left
  of it; the `launcher-*.screen` goldens and the tests reaching the launcher through `n` went in Task 11)
- Modify: `extensions/orchestrator/CMakeLists.txt`, `view/Dashboard.hpp/.cpp` (no `LauncherView`, `launcher()`,
  `launcherScope`), `view/Keys.hpp/.cpp` (`Interaction::launcher`, `launcherScope`, `openLauncherFor`), `view/HitMap.hpp`
  (drop `LauncherCandidate`, `LauncherRepo`, `LauncherKind`; renumber `MouseRows`), `view/DispatchForm.hpp/.cpp`
  (keep what the dialog and the box use — including `FormField::Permissions`, the dialog's fifth setting; drop what
  only the launcher used), `view/Sections.*`, `view/Actions.cpp`,
  `view/TextEdit.hpp` (comments), `core/EnginePoll.cpp` (comment), `guest/Driver_test.cpp`, `view/Palette_test.cpp`,
  `view/Transcript_test.cpp`, `view/Dashboard_test.cpp`, `view/DispatchForm_test.cpp`, `view/Sections_test.cpp`
  (launcher references)

**Interfaces:**
- Consumes: everything of Tasks 9–12. Produces: nothing new.

- [ ] **Step 1:** `git grep -n -i "launcher" -- extensions/orchestrator` and list every hit in the report before
  editing. Every reference either goes, or is renamed to the start dialog / task box it now means.
- [ ] **Step 2:** Delete and edit. Tests that drove the launcher are deleted when the Work view, the dialog or the
  box test the same behaviour (name the replacing test in the commit message body), and rewritten otherwise.
  This task removes code and the tests of that code only; no behaviour changes.
  An item query the launcher matched exactly (`#2375`, a URL, a Jira key) is covered by Task 5's filter test.
- [ ] **Step 3:** Build; `orchestrator_test.exe` (all) → pass; `ORCHESTRATOR_UPDATE_GOLDENS=1 … "golden screens"`
  writes nothing (`git status --short extensions/orchestrator/view/testdata` is empty);
  `ctest --test-dir out/build/clangcl-release -R orchestrator` → pass; build the guest:
  `CMAKE_BUILD_PARALLEL_LEVEL=2 cmake --build --preset clangcl-release -j 2 --target orchestrator_wasm_cxx` →
  succeeds; portability and include budget → 0; `clang-format -i`.
- [ ] **Step 4: Commit** — stage the deletions with `git rm` by name and the modified files by name; message
  `refactor(orchestrator): remove the search launcher; the Work view and n replace it`.

### Task 14: Docs — the orchestrator README, and `docs/extensions.md` keeping the user's work — Opus

**Files:**
- Modify: `extensions/orchestrator/README.md` (ordinary edit and commit)
- Modify: `docs/extensions.md` — edit via the WIP-preserving procedure the controller runs; the implementer writes
  the new text into `$S/work-board-extensions.md` instead.

- [ ] **Step 1: The README.** Rewrite **The launcher** (and its rows in the key table, the mouse paragraph and the
  finished-run row) for: the Work view (`w`, the five groups and their order, the columns, folds and sort kept
  across restarts, `/` filter, `←` `→` repository, `s` sort, `o` open, `z` fold, the details line), the start dialog
  (suggested task and its reason, the roles and `role:`, the worker default and its busy rule, `(setup)` and the
  second `⏎`, `(no profile)`, `Ctrl+P`, an item an agent works on), `n` (the box, its defaults, `Tab`, a finished
  topic run's `⏎`), the mouse table (as the spec's, plus what Task 12 observed about hover and drag), and that a
  task's `key` stays read but unused. Add under **Configuration**: `in_progress_jql` with the spec's example, and
  `role`. Under GitLab: the two REST calls per repository, how an MR's pipeline is matched by its head commit, and the
  fallback lines. Under Jira: `--fields`, the one
  key lookup per poll, and that `in_progress_jql` runs once per poll. Examples use `PRJ-1234`,
  `https://example.atlassian.net`. Commit it alone: `git add extensions/orchestrator/README.md`,
  `git diff --cached --name-only`, message `docs(orchestrator): the Work view, the start dialog and n`.
- [ ] **Step 2: `$S/work-board-extensions.md`** (Edit/Write on the scratch file only), one heading per place in
  `docs/extensions.md`, each with the exact sentence or table row to add and the line it follows:
  1. **The orchestrator overview** (`## The agent orchestrator`, ~:725): its tab lists your open merge requests and
     tickets in the Work view; `n` starts a custom task.
  2. **The `issues` key row** (~:819): `in_progress_jql` (optional, Jira only): a JQL; a ticket is in progress when
     it matches `(<jql>) AND (<in_progress_jql>)`, run once per poll; without it, when Jira's status category is
     *In Progress*; a failed query keeps that rule and shows a problem line.
  3. **The `tasks` entry table** (after ~:840): `role` — `fix_ci`, `resolve_conflicts`, `address_review` or
     `work_on_issue`: which state the start dialog suggests the task for; without it, a task whose `id` is one of
     these has that role.
  4. **GitLab** (~:821–831): replace the sentence that the list carries no pipeline with: per GitLab
     repository and poll, `glab api projects/:id/pipelines?per_page=100&order_by=id&sort=desc` and `glab api
     projects/:id/jobs?scope[]=failed&per_page=100` give each merge request's pipeline (the newest of its head
     commit) and its failed jobs; review stays inferred from `detailed_merge_status`; an MR without a matched
     pipeline, or a failed call, falls back to today's inference with a problem line.
  5. **Jira** (~:833–838): the command gains `--fields key,summary,status,issuetype,updated` (or Decision 3's
     list); tickets a merge request names but no search listed are read with one `key in (…)` search per poll.
  The file starts empty (no earlier task writes to it). After writing, grep the scratch file for a host, user or company name: none. Report the file to the controller;
  it applies it with the WIP-preserving procedure.

---

## Self-review notes

- Spec coverage: Goals 1–8 → Tasks 9, 1–3/9, 9, 9, 10, 11, 12, 4/7. Anti-goals: bounded calls (Tasks 2, 7),
  no hidden decisions (Task 10 shows the reason and every worker), keyboard parity (Task 12's `m` menu key,
  every mouse row has a key). Configuration additions (Task 4); changed commands shown on consent (Tasks 1–3
  goldens); launcher removal and its jobs moved (Tasks 11, 13; the exact-item query → Task 5 filter); screen tests
  at the spec's sizes (Tasks 9–12); docs (Task 14).
- Left to the builder → Decisions 4, 5, 11, 13 and the module layout above.
