# Orchestrator topics: implementation plan (phase 1 of 4)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Start an agent run on a free-form topic you type, not only on a pull request or an issue.

**Architecture:** `ItemRef` gains `TopicRef { slug }`. A run keeps the typed text in `Run::topic`. The engine
derives the slug, builds the prompt from `{topic}`, and checks the run out on a new branch from the
repository's base. The dispatch form gains a topic mode with two more fields: repository and topic text. The
dashboard key `n` opens that mode.

**Tech stack:** C++23, glaze (JSON), Catch2, core-cpp TUI (golden screens).

Spec: `docs/superpowers/specs/2026-11-22-orchestrator-fleet-control-design.md`, §1.

## Global constraints

- Only the slug reaches a branch name or a path. The slug is `[a-z0-9-]`, at most 40 characters, and
  `topic` when nothing survives.
- The topic text is sanitised with `singleLineNote` (at most `MaxNoteChars`; a leading `-` is refused).
- Without a configured `applies_to: topic` task, a built-in task stands in:
  `{ id: topic, label: "topic", applies_to: topic, prompt: "{topic}" }`, using the first model and no key.
- Zero warnings (`-Werror`) and clang-tidy clean. Use `enum class` and `std::expected`. C-style loops are
  not allowed.
- Goldens are rewritten only deliberately: run `ORCHESTRATOR_UPDATE_GOLDENS=1 orchestrator_test "[view]"`,
  then check each one by eye.
- Build with `-j4`.

## File map

| File | Change |
| --- | --- |
| `core/Model.hpp` | `TopicRef`; `ItemRef` gains it |
| `core/Naming.{hpp,cpp}` | `topicSlug(text)`; `slugOf(TopicRef)` → `topic-<slug>` |
| `core/Config.{hpp,cpp}` | `AppliesTo::Topic` (`topic`), `appliesToOf`, the built-in topic task |
| `core/Prompt.{hpp,cpp}` | `TopicPlaceholder`, `buildTopicPrompt(template, topic)` |
| `core/Run.{hpp,cpp}`, `core/RunJson.hpp` | `Run::topic`; persisted as item kind `topic`, `itemKey` = slug, `topic` = text |
| `core/Record.cpp` | the `agent-runs.jsonl` item for a topic: `topic:<slug>` |
| `core/Engine.hpp`, `core/EngineDispatch.cpp` | `Dispatch::topic`; the topic branch of `preview()` |
| `view/DispatchForm.{hpp,cpp}` | topic mode: `FormField::Repo`, the text field, `openTopicForm()` |
| `view/Keys.{hpp,cpp}`, `core/DashboardKeys.cpp` | `n` → `KeyCommand::NewTopic`; `d` on a finished topic run |
| `view/Sections.cpp`, `view/ViewFixtures.cpp` | the topic label in rows and details |
| `README.md`, `docs/extensions.md`, `contract/agent-runs/README.md` | the user-facing documentation |

## Task 1: Core: topic items end to end through the engine

**Interfaces produced:**
```cpp
struct TopicRef { std::string slug; bool operator==(TopicRef const&) const = default; };
using ItemRef = std::variant<ChangeRequestRef, IssueRef, JiraRef, TopicRef>;
[[nodiscard]] std::string topicSlug(std::string_view text);          // Naming.hpp
inline constexpr std::string_view TopicPlaceholder = "{topic}";      // Prompt.hpp
[[nodiscard]] std::expected<std::string, PromptError> buildTopicPrompt(std::string_view promptTemplate,
                                                                       std::string_view topic);
enum class AppliesTo : uint8_t { PullRequest, Issue, Topic };
struct Dispatch { /* … */ std::string topic; };                      // empty unless item is a TopicRef
struct Run { /* … */ std::string topic; };                           // empty unless item is a TopicRef
```

- [ ] **Tests first.**
  - `Naming_test`: a table for `topicSlug`: `"Fix the CI flake!"` → `fix-the-ci-flake`; `"  ÜBER  dash--es "`
    → `ber-dash-es`; `"!!!"` → `topic`; 60 `a`s → 40 `a`s; no trailing `-` after the cut. Also
    `itemSlug(TopicRef{"x"})` → `topic-x`.
  - `Prompt_test`: `buildTopicPrompt("{topic}", "do X")` → `do X`; `("/w {topic} now", "a")` → `/w a now`;
    `("/plan", "a")` → `/plan\na`; `("{topic}", "-x")` → `NoteStartsWithDash`; `("{topic}", "")` →
    `TopicEmpty` (a new `PromptError`).
  - `Config_test`: `applies_to: topic` parses. Without one, `tasks()` ends with the built-in topic task.
    With one, there is no built-in task.
  - `Run_test`: `toJson`/`fromJson` round-trips a topic run; a topic run with a `url` or a non-slug key is
    refused.
  - `Engine_test`: dispatching `{ item = TopicRef{}, topic = "Investigate flaky test" }` gives a run with
    id `topic-investigate-flaky-test-<stamp>`, `url` empty, `topic` kept, the base from the repository,
    and the prompt `Investigate flaky test` under the built-in task. A second dispatch of the same text
    while the first is active is refused with `AlreadyRunning`.
- [ ] **Run them:** `cmake --build out/clang-release -j4 --target orchestrator_test &&
  out/clang-release/extensions/orchestrator/orchestrator_test "[naming],[prompt],[config],[run],[engine]"`.
  Expected: they fail to compile.
- [ ] **Implement** what the file map lists for Model, Naming, Config, Prompt, Run, RunJson, Record, Engine
  and EngineDispatch. In `preview()`, when the request is for a `TopicRef`:
  - use `item = TopicRef{ topicSlug(request.topic) }`;
  - build the prompt with `buildTopicPrompt(task->prompt, request.topic)`;
  - leave `url` empty;
  - set `run.topic` to the sanitised text;
  - take the base as an issue's: `repo->base`, else the default branch.
- [ ] **Run them again.** Expected: PASS, with the whole `orchestrator_test` green.
- [ ] **Commit:** `feat(orchestrator): a run on a topic you type`.

## Task 2: The topic form, the `n` key, labels and docs

**Interfaces produced:**
```cpp
enum class FormField : uint8_t { Task = 0, Model, Note, Repo };   // Note holds a topic form's text
[[nodiscard]] std::optional<DispatchFormState> openTopicForm(Engine const& engine,
                                                             std::string_view repo,
                                                             std::string topic,
                                                             std::string session);
// KeyCommand::NewTopic, bound to `n` on the dashboard.
```

- [ ] **Tests first.**
  - `DispatchForm_test`:
    - `openTopicForm` focuses the text field;
    - typing fills it, and Enter is inert while it is empty;
    - `←`/`→` on Repo cycle the configured repositories, and on Task only the topic tasks;
    - Enter gives a `DispatchAction` with `item = TopicRef`, `topic` set and `note` empty.
  - `Keys_test`: `n` opens the topic form on the selected row's repository, or the first one. `d` on a
    finished topic run opens it with the run's text and repository.
  - `Dashboard_test` goldens: the topic form at 120×40 and 80×24, and a topic run's row and details.
- [ ] **Run them.** Expected: they fail.
- [ ] **Implement:**
  - In topic mode the form's first line reads `topic · <text>`. The fields run Repo, Topic, Task, Model.
    The checkout line reads `new branch agent/topic-<slug>-…`.
  - `itemLabel(TopicRef)` is the text, drawn sanitised and cut. The details show `topic` where an item
    shows its URL.
  - Reserve `n` in `DashboardKeys`. A configuration whose task key is `n` is now refused with the existing
    message.
- [ ] **Write the goldens:** `ORCHESTRATOR_UPDATE_GOLDENS=1 …orchestrator_test "[view]"`, then check each by
  eye.
- [ ] **Documentation:**
  - README: the `n` key row, a "Topics" paragraph, and `{topic}`;
  - `docs/extensions.md`: `applies_to: topic`;
  - `contract/agent-runs/README.md`: the `topic` item kind.
- [ ] **Run the whole `orchestrator_test`.** Expected: PASS.
- [ ] **Commit:** `feat(orchestrator): n starts an agent on a topic`.
