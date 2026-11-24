// SPDX-License-Identifier: Apache-2.0
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/view/Launcher.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;

namespace
{
[[nodiscard]] std::vector<std::string> labelsOf(std::vector<Candidate> const& candidates)
{
    return candidates | std::views::transform([](Candidate const& candidate) {
               return std::holds_alternative<TopicRef>(candidate.item) ? "topic:" + candidate.title
                                                                       : itemLabel(candidate.item);
           })
           | std::ranges::to<std::vector>();
}
} // namespace

TEST_CASE("the launcher lists what needs attention first, then the pull requests, then the issues",
          "[orchestrator][view][launcher]")
{
    auto const engine = bootEngine(World {});
    auto const candidates = candidatesOf(engine, "");
    auto const requests = engine.changeRequests("contour");
    auto const issues = engine.unclaimedIssues("contour");
    REQUIRE(candidates.size() == requests.size() + issues.size());
    CHECK(std::ranges::is_sorted(candidates, {}, &Candidate::group));
    CHECK(std::ranges::none_of(candidates,
                               [](Candidate const& c) { return c.group == CandidateGroup::Topic; }));
    for (auto const& candidate: candidates)
    {
        INFO(itemLabel(candidate.item));
        auto const needs =
            candidate.ci == CiState::Failing || candidate.review == ReviewState::ChangesRequested;
        if (std::holds_alternative<ChangeRequestRef>(candidate.item))
            CHECK(candidate.group
                  == (needs ? CandidateGroup::NeedsAttention : CandidateGroup::MyPullRequests));
        else
            CHECK(candidate.group == CandidateGroup::Issues);
        CHECK(candidate.repo == "contour");
        CHECK_FALSE(candidate.url.empty());
    }
    // The fixture's two pull requests with failing checks need attention.
    CHECK((labelsOf(candidates) | std::views::take(2) | std::ranges::to<std::vector>())
          == std::vector<std::string> { "#2114", "#1847" });
    // Blanks alone are no query.
    CHECK(candidatesOf(engine, "   ") == candidates);
}

TEST_CASE("a query filters the items, and the topic row stands first unless an item is named exactly",
          "[orchestrator][view][launcher]")
{
    auto const engine = bootEngine(World {});

    auto const flaky = candidatesOf(engine, "FLAKY");
    REQUIRE_FALSE(flaky.empty());
    CHECK(flaky.front().group == CandidateGroup::Topic);
    CHECK(flaky.front().title == "FLAKY");
    CHECK(labelsOf(flaky) == std::vector<std::string> { "topic:FLAKY", "#2338" });

    for (auto const query: { "2375"sv, "#2375"sv, "https://github.com/contour-terminal/contour/pull/2375"sv })
    {
        INFO(query);
        auto const named = candidatesOf(engine, query);
        REQUIRE(named.size() >= 2);
        CHECK(itemLabel(named.front().item) == "#2375");
        CHECK(named[1].group == CandidateGroup::Topic);
    }

    // Nothing matches: the topic row alone.
    CHECK(labelsOf(candidatesOf(engine, " zzz-nothing "))
          == std::vector<std::string> { "topic:zzz-nothing" });
}

TEST_CASE("an item an agent works on says which run", "[orchestrator][view][launcher]")
{
    auto const engine = bootEngine(
        World { .stored = { storedRun(Running {}, "pr-2375-20261113-150847", ChangeRequestRef { 2375 }) } });
    auto const candidates = candidatesOf(engine, "#2375");
    REQUIRE_FALSE(candidates.empty());
    CHECK(candidates.front().activeRun == "pr-2375-20261113-150847");
    CHECK(std::ranges::all_of(candidates | std::views::drop(1),
                              [](Candidate const& c) { return c.activeRun.empty(); }));
}

namespace
{
constexpr auto SessionOne = "00000001-0000-4000-8000-000000000000"sv;

/// @return The launcher opened on @p item, as an optional the keys take.
[[nodiscard]] std::optional<LauncherState> openedOn(Engine const& engine,
                                                    ItemRef const& item,
                                                    std::string_view task = {})
{
    auto opened = openLauncherOn(engine, "contour", item, task, std::string { SessionOne });
    return opened ? std::optional { std::move(*opened) } : std::nullopt;
}

[[nodiscard]] core::tui::KeyEvent typed(char32_t c)
{
    return core::tui::KeyEvent { .key = static_cast<core::tui::KeyCode>(c), .codepoint = c };
}

[[nodiscard]] core::tui::KeyEvent special(core::tui::KeyCode code,
                                          core::tui::Modifier modifiers = core::tui::Modifier::None)
{
    return core::tui::KeyEvent { .key = code, .modifiers = modifiers };
}

/// Types @p text into @p launcher, key by key.
void type(std::optional<LauncherState>& launcher, Engine const& engine, std::string_view text)
{
    for (auto const c: text)
        CHECK_FALSE(
            applyLauncherKey(typed(static_cast<char32_t>(c)), launcher, engine, HooksWarning::None, "t1"));
}

[[nodiscard]] std::optional<DispatchAction> press(std::optional<LauncherState>& launcher,
                                                  Engine const& engine,
                                                  core::tui::KeyCode code,
                                                  HooksWarning hooksAtInput = HooksWarning::None)
{
    return applyLauncherKey(special(code), launcher, engine, hooksAtInput, "t1");
}
} // namespace

TEST_CASE("the launcher's query is typed, and Enter on the topic row starts a topic",
          "[orchestrator][view][launcher]")
{
    using core::tui::KeyCode;
    auto const engine = bootEngine(World {});
    auto launcher = openLauncher(engine, "", std::string { SessionOne });
    REQUIRE(launcher.has_value());
    CHECK(launcher->form.has_value()); // the first item, needing attention

    type(launcher, engine, "flakyx");
    CHECK_FALSE(press(launcher, engine, KeyCode::Backspace));
    REQUIRE(launcher.has_value());
    CHECK(launcher->query == "flaky");
    CHECK(launcher->highlighted == 0);
    REQUIRE(launcher->form.has_value());
    CHECK(std::holds_alternative<TopicRef>(launcher->form->item));
    CHECK(launcher->form->topic == "flaky");

    auto const action = press(launcher, engine, KeyCode::Enter);
    REQUIRE(action.has_value());
    CHECK_FALSE(launcher.has_value());
    CHECK(std::holds_alternative<TopicRef>(action->item));
    CHECK(action->topic == "flaky");
    CHECK(action->repo == "contour");
    CHECK(action->permissions == "standard");
    CHECK(action->session == SessionOne);
    CHECK(action->nearTab == "t1");
}

TEST_CASE("the launcher's query takes the line-editing keys", "[orchestrator][view][launcher]")
{
    using core::tui::KeyCode;
    using core::tui::Modifier;
    auto const engine = bootEngine(World {});
    auto launcher = openLauncher(engine, "", std::string { SessionOne });
    REQUIRE(launcher.has_value());
    type(launcher, engine, "investigate the flaky test");
    auto const ctrl = [](char32_t letter) {
        return core::tui::KeyEvent { .key = static_cast<KeyCode>(letter),
                                     .modifiers = Modifier::Ctrl,
                                     .codepoint = letter };
    };
    CHECK_FALSE(applyLauncherKey(ctrl(U'w'), launcher, engine, HooksWarning::None, "t1"));
    REQUIRE(launcher.has_value());
    CHECK(launcher->query == "investigate the flaky ");
    REQUIRE(launcher->form.has_value());
    CHECK(launcher->form->topic == "investigate the flaky");
    // Alt+Backspace drops a word, rather than reading as Esc and closing the launcher.
    CHECK_FALSE(
        applyLauncherKey(core::tui::KeyEvent { .key = KeyCode::Backspace, .modifiers = Modifier::Alt },
                         launcher,
                         engine,
                         HooksWarning::None,
                         "t1"));
    REQUIRE(launcher.has_value());
    CHECK(launcher->query == "investigate the ");
    CHECK_FALSE(applyLauncherKey(ctrl(U'u'), launcher, engine, HooksWarning::None, "t1"));
    REQUIRE(launcher.has_value());
    CHECK(launcher->query.empty());
}

TEST_CASE("the launcher's highlight moves, and Enter on an item starts its first task",
          "[orchestrator][view][launcher]")
{
    using core::tui::KeyCode;
    auto const engine = bootEngine(World {});
    auto launcher = openLauncher(engine, "flaky", std::string { SessionOne });
    REQUIRE(launcher.has_value());
    CHECK_FALSE(press(launcher, engine, KeyCode::Down));
    REQUIRE(launcher->form.has_value());
    CHECK(launcher->form->item == ItemRef { IssueRef { 2338 } });
    // Up from the first row wraps to the last.
    CHECK_FALSE(press(launcher, engine, KeyCode::Up));
    CHECK_FALSE(press(launcher, engine, KeyCode::Up));
    CHECK(launcher->highlighted == 1);

    auto const action = press(launcher, engine, KeyCode::Enter);
    REQUIRE(action.has_value());
    CHECK(action->item == ItemRef { IssueRef { 2338 } });
    CHECK(action->task == "issue");
    CHECK(action->model == "opus");
    CHECK_FALSE(action->note.has_value());
}

TEST_CASE("the launcher opens on an item, and its settings change with Tab and the arrows",
          "[orchestrator][view][launcher]")
{
    using core::tui::KeyCode;
    auto const engine = bootEngine(World {});
    auto launcher = openedOn(engine, ChangeRequestRef { 2375 });
    REQUIRE(launcher.has_value());
    CHECK(launcher->query == "#2375");
    CHECK(launcher->highlighted == 0);
    REQUIRE(launcher->form.has_value());
    CHECK(launcher->form->item == ItemRef { ChangeRequestRef { 2375 } });

    CHECK_FALSE(press(launcher, engine, KeyCode::Tab));
    CHECK(launcher->focus == LauncherFocus::Settings);
    CHECK(launcher->setting == FormField::Task);
    CHECK_FALSE(press(launcher, engine, KeyCode::Right)); // the only pull-request task: still fix CI
    CHECK_FALSE(press(launcher, engine, KeyCode::Tab));
    CHECK(launcher->setting == FormField::Model);
    CHECK_FALSE(press(launcher, engine, KeyCode::Right)); // sonnet → opus
    CHECK_FALSE(press(launcher, engine, KeyCode::Tab));
    CHECK(launcher->setting == FormField::Permissions);
    CHECK_FALSE(press(launcher, engine, KeyCode::Right)); // trusted
    CHECK_FALSE(press(launcher, engine, KeyCode::Tab));
    CHECK(launcher->setting == FormField::Note);
    type(launcher, engine, "small");
    CHECK(launcher->form->note == "small");
    CHECK(launcher->query == "#2375");
    CHECK_FALSE(press(launcher, engine, KeyCode::Tab));
    CHECK(launcher->focus == LauncherFocus::List);

    auto const action = press(launcher, engine, KeyCode::Enter);
    REQUIRE(action.has_value());
    CHECK(action->task == "fix_ci");
    CHECK(action->model == "opus");
    CHECK(action->permissions == "trusted");
    CHECK(action->note == std::optional<std::string> { "small" });

    // A task to start with, as a task's key and a finished run's `d` ask.
    auto const chosen = openedOn(engine, ChangeRequestRef { 2375 }, "fix_ci");
    REQUIRE(chosen.has_value());
    REQUIRE(chosen->form.has_value());
    auto const chosenAction = actionOf(*chosen->form, engine, "t1");
    CHECK(chosenAction.task == "fix_ci");
    CHECK(chosenAction.model == "sonnet");

    CHECK(openLauncherOn(engine, "contour", ChangeRequestRef { 1 }, "", std::string { SessionOne }).error()
          == LauncherRefusal::NotListed);
}

TEST_CASE("the launcher closes on Esc, and Ctrl+P shows more", "[orchestrator][view][launcher]")
{
    using core::tui::KeyCode;
    using core::tui::Modifier;
    auto const engine = bootEngine(World {});
    auto launcher = openLauncher(engine, "x", std::string { SessionOne });
    CHECK_FALSE(applyLauncherKey(core::tui::KeyEvent { .key = static_cast<KeyCode>(U'p'),
                                                       .modifiers = Modifier::Ctrl,
                                                       .codepoint = U'p' },
                                 launcher,
                                 engine,
                                 HooksWarning::None,
                                 "t1"));
    REQUIRE(launcher.has_value());
    CHECK(launcher->preview == LauncherPreview::Full);
    CHECK(launcher->query == "x");
    CHECK_FALSE(press(launcher, engine, KeyCode::Escape));
    CHECK_FALSE(launcher.has_value());

    launcher = openLauncher(engine, "x", std::string { SessionOne });
    CHECK_FALSE(
        applyLauncherKey(special(KeyCode::Enter, Modifier::Alt), launcher, engine, HooksWarning::None, "t1"));
    CHECK_FALSE(launcher.has_value());
}

TEST_CASE("the launcher refuses an item an agent works on, and asks twice while the setup is incomplete",
          "[orchestrator][view][launcher]")
{
    using core::tui::KeyCode;
    auto const busy = bootEngine(
        World { .stored = { storedRun(Running {}, "pr-2375-20261113-150847", ChangeRequestRef { 2375 }) } });
    auto launcher = openedOn(busy, ChangeRequestRef { 2375 });
    REQUIRE(launcher.has_value());
    CHECK_FALSE(press(launcher, busy, KeyCode::Enter));
    CHECK(launcher.has_value());
    REQUIRE(launcher->form.has_value());
    CHECK(previewOf(*launcher->form, busy).error() == DispatchRefusal::AlreadyRunning);

    auto const incomplete = bootEngine(World { .hooks = "contour/setup-hooks-missing.json" });
    launcher = openLauncher(incomplete, "topic", std::string { SessionOne });
    REQUIRE(launcher.has_value());
    CHECK_FALSE(press(launcher, incomplete, KeyCode::Enter, HooksWarning::Shown));
    REQUIRE(launcher.has_value());
    REQUIRE(launcher->form.has_value());
    CHECK(launcher->hooks == HooksWarning::Confirmed);
    // The warning survives a move of the highlight.
    CHECK_FALSE(press(launcher, incomplete, KeyCode::Down));
    CHECK_FALSE(press(launcher, incomplete, KeyCode::Up));
    CHECK(launcher->hooks == HooksWarning::Confirmed);
    CHECK(press(launcher, incomplete, KeyCode::Enter, HooksWarning::Confirmed).has_value());
}

namespace
{
/// Three workers: two Claude workers on their own sets, one of which lacks its hooks, and a Copilot one;
/// a pull-request task on each.
constexpr auto WorkersConfig = R"({ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "workers": [ { "id": "complete", "agent": "claude", "env_set": "complete" },
               { "id": "lacking", "agent": "claude", "env_set": "lacking" },
               { "id": "pilot", "agent": "copilot" } ],
  "repos": [ { "name": "contour", "path": "~/projects/contour", "forge": "github", "issues": [] } ],
  "tasks": [
    { "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
      "worker": "complete", "prompt": "/contour-workflows:fix-ci {url}" },
    { "id": "review", "key": "v", "label": "review", "applies_to": "pr", "model": "sonnet",
      "worker": "lacking", "prompt": "/contour-workflows:review {url}" },
    { "id": "pilot", "key": "y", "label": "pilot", "applies_to": "pr", "model": "sonnet",
      "worker": "pilot", "prompt": "fix the CI of {url}" } ] })"sv;
} // namespace

TEST_CASE("the launcher warns only of the setup of the task's own worker",
          "[orchestrator][view][launcher][workers]")
{
    using core::tui::KeyCode;
    auto const engine =
        bootEngine(World { .configJson = std::string { WorkersConfig },
                           .hooksBySet = { { "lacking", "contour/setup-hooks-missing.json" } } });
    REQUIRE(engine.config() != nullptr);
    REQUIRE(engine.hooks() == HooksState::Missing);

    for (auto const task: { "fix_ci"sv, "pilot"sv })
    {
        CAPTURE(task);
        auto launcher = openedOn(engine, ChangeRequestRef { 2375 }, task);
        REQUIRE(launcher.has_value());
        REQUIRE(launcher->form.has_value());
        CHECK(launcher->hooks == HooksWarning::None);
        auto const* const worker = workerOf(*launcher->form, engine);
        REQUIRE(worker != nullptr);
        CHECK(setupWarnings(engine, *worker).empty());
        CHECK(press(launcher, engine, KeyCode::Enter).has_value());
    }

    auto launcher = openedOn(engine, ChangeRequestRef { 2375 }, "review");
    REQUIRE(launcher.has_value());
    REQUIRE(launcher->form.has_value());
    CHECK(launcher->hooks == HooksWarning::Shown);
    CHECK(setupWarnings(engine, *workerOf(*launcher->form, engine))
          == std::vector { std::string { HooksMissingWarning } });
    CHECK_FALSE(press(launcher, engine, KeyCode::Enter, HooksWarning::Shown));
    CHECK(press(launcher, engine, KeyCode::Enter, HooksWarning::Confirmed).has_value());
}

TEST_CASE("a Copilot task's launcher offers no permission profile", "[orchestrator][view][launcher][workers]")
{
    using core::tui::KeyCode;
    auto const engine = bootEngine(World { .configJson = std::string { WorkersConfig } });
    // The settings Tab visits from the list, in order, until it is back on the list.
    auto const visited = [&](std::string_view task) {
        auto launcher = openedOn(engine, ChangeRequestRef { 2375 }, task);
        auto fields = std::vector<FormField> {};
        REQUIRE(launcher.has_value());
        CHECK_FALSE(press(launcher, engine, KeyCode::Tab));
        while (launcher->focus == LauncherFocus::Settings && fields.size() < 8)
        {
            fields.push_back(launcher->setting);
            CHECK_FALSE(press(launcher, engine, KeyCode::Tab));
        }
        return fields;
    };
    CHECK(std::ranges::contains(visited("fix_ci"), FormField::Permissions));
    auto const copilot = visited("pilot");
    CHECK_FALSE(copilot.empty());
    CHECK_FALSE(std::ranges::contains(copilot, FormField::Permissions));
}

namespace
{
/// Two repositories, each listing the same recorded pull requests and issues.
[[nodiscard]] World twoRepositories()
{
    return World {
        .repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github",
                        "issues": [ { "source": "forge", "query": "assignee:@me" } ] },
                      { "name": "other", "path": "~/projects/other", "forge": "github",
                        "issues": [ { "source": "forge", "query": "assignee:@me" } ] } ])",
        .repoPaths = { std::string { ContourPath }, std::string { OtherPath } },
    };
}

[[nodiscard]] core::tui::KeyEvent arrow(core::tui::KeyCode code, core::tui::Modifier modifiers = {})
{
    return core::tui::KeyEvent { .key = code, .modifiers = modifiers };
}
} // namespace

TEST_CASE("the launcher's scope narrows the list to a repository and a kind",
          "[orchestrator][view][launcher]")
{
    auto const engine = bootEngine(twoRepositories());
    auto const every = candidatesOf(engine, "");
    REQUIRE(std::ranges::any_of(every, [](Candidate const& c) { return c.repo == "contour"; }));
    REQUIRE(std::ranges::any_of(every, [](Candidate const& c) { return c.repo == "other"; }));

    auto const other = candidatesOf(engine, "", LauncherScope { .repo = "other" });
    REQUIRE_FALSE(other.empty());
    CHECK(std::ranges::all_of(other, [](Candidate const& c) { return c.repo == "other"; }));
    CHECK(other.size() * 2 == every.size());

    auto const issues = candidatesOf(engine, "", LauncherScope { .kind = KindScope::Issues });
    REQUIRE_FALSE(issues.empty());
    CHECK(std::ranges::all_of(issues, [](Candidate const& c) { return c.group == CandidateGroup::Issues; }));
    auto const attention = candidatesOf(engine, "", LauncherScope { .kind = KindScope::NeedsAttention });
    REQUIRE_FALSE(attention.empty());
    CHECK(std::ranges::all_of(attention,
                              [](Candidate const& c) { return c.group == CandidateGroup::NeedsAttention; }));
    auto const prs = candidatesOf(engine, "", LauncherScope { .kind = KindScope::PullRequests });
    CHECK(std::ranges::none_of(prs, [](Candidate const& c) { return c.group == CandidateGroup::Issues; }));

    // A typed query keeps its topic row whatever the kind.
    auto const typed = candidatesOf(engine, "flaky", LauncherScope { .kind = KindScope::NeedsAttention });
    CHECK(std::ranges::any_of(typed, [](Candidate const& c) { return c.group == CandidateGroup::Topic; }));
}

TEST_CASE("left and right step the launcher's repository, ctrl the kind; a topic starts in the repository",
          "[orchestrator][view][launcher]")
{
    using core::tui::KeyCode;
    using core::tui::Modifier;
    auto const engine = bootEngine(twoRepositories());
    auto launcher = openLauncher(engine, "", std::string { SessionOne });
    REQUIRE(launcher.has_value());
    auto const step = [&](core::tui::KeyEvent const& key) {
        CHECK_FALSE(applyLauncherKey(key, launcher, engine, HooksWarning::None, "t1"));
        REQUIRE(launcher.has_value());
    };
    step(arrow(KeyCode::Right));
    CHECK(launcher->scope.repo == "contour");
    step(arrow(KeyCode::Right));
    CHECK(launcher->scope.repo == "other");
    step(arrow(KeyCode::Right));
    CHECK(launcher->scope.repo.empty());
    step(arrow(KeyCode::Left));
    CHECK(launcher->scope.repo == "other");
    step(arrow(KeyCode::Right, Modifier::Ctrl));
    CHECK(launcher->scope.kind == KindScope::NeedsAttention);
    step(arrow(KeyCode::Left, Modifier::Ctrl));
    step(arrow(KeyCode::Left, Modifier::Ctrl));
    CHECK(launcher->scope.kind == KindScope::Issues);

    type(launcher, engine, "bump the vendored copy");
    REQUIRE(launcher->form.has_value());
    CHECK(launcher->form->repo == "other");
    auto const action = press(launcher, engine, KeyCode::Enter);
    REQUIRE(action.has_value());
    CHECK(action->repo == "other");
}

TEST_CASE("the launcher opens on a repository no longer configured as listing every one",
          "[orchestrator][view][launcher]")
{
    auto const engine = bootEngine(World {});
    auto const launcher =
        openLauncher(engine, "", {}, LauncherScope { .repo = "gone", .kind = KindScope::Issues });
    REQUIRE(launcher.has_value());
    CHECK(launcher->scope.repo.empty());
    CHECK(launcher->scope.kind == KindScope::Issues);
}
