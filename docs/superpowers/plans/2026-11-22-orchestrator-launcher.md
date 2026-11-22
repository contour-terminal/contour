# Orchestrator launcher: implementation plan (Fleet + Focus, phase 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** One popup, `n`, browses the pull requests and issues and starts an agent on one of them, or on a
topic typed into it, with the task, model, repository and permission profile chosen in the same place.

**Why this comes first:** the spec placed the launcher third. The Fleet + Focus layout removes the item lists
from the dashboard and turns `d` into the Details tab. Until the launcher exists, those lists and `d` are the
only way to start an agent on an item, so the launcher must land first. Each phase then stays usable on its own.

**Architecture:**
- `view/Launcher.{hpp,cpp}` holds the logic:
  - the candidates, a pure function of the engine's lists and the query;
  - the launcher's state;
  - its keys.
- The highlighted candidate and the chosen settings become the `DispatchFormState` the existing
  `previewOf()`/`actionOf()` take, so the preview, the refusals and the hooks warning are reused.
- `LauncherView` replaces `DispatchFormView` on screen.
- `Dispatch` gains its `permissions` again, so the launcher can choose a profile.

**Tech stack:** C++23, Catch2, core-cpp TUI (golden screens).

Spec: `docs/superpowers/specs/2026-11-22-orchestrator-fleet-focus-design.md`, "The launcher".

## Global constraints

- Every printable key types into the query: the launcher has no letter commands.
- `esc` closes the launcher and loses nothing outside it.
- The item lists stay on the dashboard in this phase; the Fleet + Focus phase removes them.
- A task's `key` stays valid in the configuration. On the dashboard it opens the launcher on the selected item
  with that task chosen: an accelerator, shown by the palette later.
- Zero warnings, clang-tidy clean, `enum class` and `std::expected`. Goldens are rewritten only deliberately.
  Build with `-j4`.

## File map

| File | Change |
| --- | --- |
| `view/Launcher.{hpp,cpp}` | **new:** `Candidate`, `CandidateGroup`, `candidatesOf()`, `LauncherState`, `openLauncher()`, `applyLauncherKey()`, `LauncherView` |
| `view/Launcher_test.cpp` | **new** |
| `view/DispatchForm.{hpp,cpp}` | `DispatchFormState` gains `permissions`; the form's view and topic form go, while its preview, action and refusal logic stay as the launcher's |
| `view/Keys.{hpp,cpp}`, `view/Dashboard.{hpp,cpp}` | `n`, `d`, `⏎` on an item, the task keys and `d` on a finished run open the launcher; `Interaction::launcher` replaces `form` |
| `core/Engine.hpp`, `core/EngineDispatch.cpp`, `guest/Driver.cpp` | `Dispatch::permissions`, `DispatchRefusal::UnknownProfile` |
| goldens | the launcher at 120×40 and 80×24, with an empty query, a typed query and a warning |
| `README.md` | the keys, and the launcher |

## Task 1: Candidates

**Interfaces produced:**

```cpp
enum class CandidateGroup : uint8_t { Topic = 0, NeedsAttention, MyPullRequests, Issues };
struct Candidate {
    CandidateGroup group;
    std::string repo;
    ItemRef item;            // a TopicRef for the topic row
    std::string url, title;  // title: sanitised; the topic row's: the query
    CiState ci = CiState::None;
    ReviewState review = ReviewState::None;
    std::vector<std::string> failingChecks;
    std::string activeRun;   // the id of an active run on the item; empty: none
};
[[nodiscard]] std::vector<Candidate> candidatesOf(Engine const& engine, std::string_view query);
```

- [ ] **Tests first** (`Launcher_test`, `[launcher]`, from the view fixtures' lists):
  - **Empty query:**
    - pull requests with failing CI or requested changes come first (NeedsAttention), then the other pull
      requests, then the issues, each in the engine's order;
    - there is no topic row.
  - **Query `reflow`:** the topic row first, then the items whose title holds it, case folded.
  - **Query `2375`, `#2375`, the item's URL or a Jira key:** that item first, then the topic row.
  - **Active runs:** an item with an active run has `activeRun` set.
  - **A query of spaces:** the same as an empty one.
- [ ] **Implement** `candidatesOf`. Matching is a case-folded substring of `#<number> <title>`, or the Jira key;
  an exact match is the number, `#number`, the URL, or the key.
- [ ] **Run** `orchestrator_test "[launcher]"`, then the whole suite.
- [ ] **Commit:** `feat(orchestrator): the launcher's candidates`.

## Task 2: The launcher's state and keys

**Interfaces produced:**

```cpp
enum class LauncherFocus : uint8_t { List = 0, Task, Model, Repo, Permissions };
struct LauncherState {
    std::string query;
    std::size_t highlighted = 0;          // into candidatesOf(engine, query)
    LauncherFocus focus = LauncherFocus::List;
    std::optional<std::size_t> task, model, repo, permissions;  // chosen with ◂▸; none: the candidate's default
    std::string session;
    HooksWarning hooks = HooksWarning::None;
    Confirm again = Confirm::None;        // an item with an active run, or a setup warning, asks for a second ⏎
    Preview preview = Preview::Short;     // alt+p: Full shows the prompt and the command line
};
[[nodiscard]] std::optional<LauncherState> openLauncher(Engine const&, std::string query, std::string session);
[[nodiscard]] std::optional<LauncherState> openLauncherOn(Engine const&, std::string_view repo, ItemRef const&,
                                                          std::optional<std::string_view> task, std::string session);
[[nodiscard]] std::optional<DispatchFormState> formOf(LauncherState const&, Engine const&);
[[nodiscard]] std::optional<DispatchAction> applyLauncherKey(core::tui::KeyEvent const&, std::optional<LauncherState>&,
                                                             Engine const&, HooksWarning hooksAtInput,
                                                             std::string_view nearTab);
```

- [ ] **Tests first:**
  - Typing extends the query and resets the highlight to 0. Backspace shortens it.
  - `↑↓` move the highlight; `tab` moves into the settings line and back; `←→` there cycle the task (only the
    tasks that apply to the candidate's kind), the model, the repository (topics only) and the profile.
  - `⏎` on an item gives a `DispatchAction` with that item, its task's default model, and the default profile.
  - On the topic row it gives a topic dispatch of the query.
  - On an item whose `activeRun` is set, the first `⏎` only asks (`again`) and the second dispatches.
  - Missing hooks ask once, as the form does today.
  - `esc` resets it, and so does a key with Alt.
  - `alt+p` toggles the full preview.
  - `openLauncherOn` highlights the item, with the query its number.
- [ ] **Implement**, reusing `previewOf()`/`actionOf()` through `formOf()`. `DispatchFormState` gains
  `permissions` (an index into `Config::profiles()`), and `Dispatch::permissions` with `UnknownProfile` return.
- [ ] **Commit:** `feat(orchestrator): the launcher's keys`.

## Task 3: Draw it, and put it on the dashboard

- [ ] **Tests first:**
  - `Launcher_test` goldens:
    - `launcher-empty-120x40`, `launcher-query-120x40`, `launcher-empty-80x24`;
    - `launcher-warning-120x40` (missing hooks), and the second-`⏎` question for an item with an agent.
  - `Keys_test`:
    - `n` opens it empty;
    - `⏎` and `d` on an item row open it on the item;
    - a task key opens it on the item with that task;
    - `d` on a finished run opens it on the run's item, task and model.
  - Existing dispatch tests are moved to the launcher, not deleted: every behaviour the form had has a test.
- [ ] **Implement** `LauncherView`:
  - a rounded box, `✦ New agent`;
  - the query field;
  - the groups, with CI, review and agent glyph columns, and the matched text underlined in the accent;
  - the detail line;
  - the settings line;
  - the preview line;
  - the warning line.

  Replace `DispatchFormView` and `Interaction::form` with the launcher, and remove the form's own view, its
  topic form and its now unused key handling.
- [ ] **README:** the key table (`n` new agent, task keys as accelerators), and the launcher paragraph.
- [ ] **Run** the whole `orchestrator_test`, plus `vthost_test "[agents]"`, which dispatches through the
  dashboard. Expected: PASS.
- [ ] **Commit:** `feat(orchestrator): n opens the launcher, which replaces the dispatch form`.

## Verification

- [ ] `/simplify` over the phase.
- [ ] `vthost_test` whole, `vtextension_test`, and the `contour` build.
