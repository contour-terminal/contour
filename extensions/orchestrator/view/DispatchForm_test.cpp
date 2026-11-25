// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/view/Dashboard.hpp>
#include <orchestrator/view/DispatchForm.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/StartDialog.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;

namespace
{

/// A world whose `contour agent setup` dry run finds no `~/.claude` at all.
[[nodiscard]] World hooksMissing()
{
    return World { .hooks = "contour/setup-claude-absent.json" };
}

/// A world whose four agent slots are all taken.
[[nodiscard]] World limitReached()
{
    return World { .stored = {
                       storedRun(Running {}, "pr-2369-20261113-150847", ChangeRequestRef { 2369 }),
                       storedRun(Running {}, "pr-2346-20261113-150848", ChangeRequestRef { 2346 }),
                       storedRun(Running {}, "pr-2114-20261113-150849", ChangeRequestRef { 2114 }),
                       storedRun(Running {}, "pr-1847-20261113-150850", ChangeRequestRef { 1847 }),
                   } };
}

/// @return The dispatches among @p actions.
[[nodiscard]] std::vector<DispatchAction> dispatchesIn(std::vector<UiAction> const& actions)
{
    auto found = std::vector<DispatchAction> {};
    for (auto const& action: actions)
        if (auto const* const dispatch = std::get_if<DispatchAction>(&action))
            found.push_back(*dispatch);
    return found;
}

/// Opens the start dialog on #2375, on fix CI.
void openOn2375(DashboardFixture& fixture)
{
    fixture.dashboard.openStartDialogOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
}

/// @return The open start dialog; it must be open.
[[nodiscard]] StartDialogState const& dialogOf(DashboardFixture const& fixture)
{
    REQUIRE(fixture.dashboard.interaction().startDialog.has_value());
    return *fixture.dashboard.interaction().startDialog;
}

/// @return Whether the start dialog is open.
[[nodiscard]] bool isDialogOpen(DashboardFixture const& fixture)
{
    return fixture.dashboard.interaction().startDialog.has_value();
}

} // namespace

TEST_CASE("the form's session comes from the dashboard's source", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    openOn2375(fixture);
    CHECK(dialogOf(fixture).form.session == "00000001-0000-4000-8000-000000000000");
    CHECK(fixture.ids.taken == 1);
}

TEST_CASE("a note starting with - is refused in the form", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    openOn2375(fixture);
    CHECK(fixture.dashboard.input("\t\t\t\t-rf").empty());
    REQUIRE(dialogOf(fixture).setting == FormField::Note);
    CHECK(dialogOf(fixture).form.note == "-rf");
    CHECK(fixture.screen().contains("a note may not start with -"));

    // Enter does nothing while it does, and the form stays open.
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(isDialogOpen(fixture));

    // Once it no longer starts with one, Enter dispatches it.
    CHECK(fixture.dashboard
              .input("\x7f\x7f\x7f"
                     "keep it small")
              .empty());
    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().note == std::optional<std::string> { "keep it small" });
    CHECK_FALSE(isDialogOpen(fixture));
}

TEST_CASE("the command line equals the template substituted", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    openOn2375(fixture);
    auto const& form = dialogOf(fixture).form;
    auto const preview = previewOf(form, fixture.engine);
    REQUIRE(preview.has_value());

    // What pane-open is given: the template, its placeholders filled in from the run the dispatch stores.
    auto const* const pane = findPaneTemplate(AgentPaneTemplate);
    REQUIRE(pane != nullptr);
    auto const argv = substitute(pane->argv, agentPaneArguments(preview->run));
    REQUIRE(argv.has_value());
    CHECK(*argv == preview->agentArgv);
    CHECK(argv->front() == "claude");
    CHECK(std::ranges::contains(*argv, form.session));

    // Ctrl+P shows that argv as one line a shell reads back word for word.
    CHECK(fixture.dashboard.input("\x10").empty());
    auto const screen = fixture.screen();
    auto const line = shellQuoted(*argv);
    CHECK(line.starts_with("claude --model sonnet --session-id "));
    CHECK(screen.contains("claude --model sonnet --session-id"));
    CHECK(screen.contains("'/contour-workflows:fix-ci"));
}

TEST_CASE("hooks missing needs a second Enter", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { hooksMissing() };
    openOn2375(fixture);
    REQUIRE(dialogOf(fixture).hooks == HooksWarning::Shown);
    CHECK(fixture.screen().contains("status hooks are not installed"));

    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(dialogOf(fixture).hooks == HooksWarning::Confirmed);
    CHECK(fixture.screen().contains("enter starts it anyway"));

    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().nearTab == DashboardFixture::Tab);
}

TEST_CASE("two Enters in one read do not confirm the hooks warning", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { hooksMissing() };
    openOn2375(fixture);

    // A paste, or a key held down: both Enters come in one read, and the second asks nothing.
    CHECK(fixture.dashboard.input("\r\r").empty());
    CHECK(fixture.dashboard.input("\r").size() == 1); // a later read dispatches
    CHECK_FALSE(isDialogOpen(fixture));
}

TEST_CASE("esc cancels the form and asks nothing", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    openOn2375(fixture);
    CHECK(fixture.dashboard.input("\x1b").empty());
    fixture.escapeTimesOut();
    CHECK_FALSE(isDialogOpen(fixture));
    CHECK_FALSE(fixture.screen().contains("Start agent"));
}

TEST_CASE("the form cycles the tasks that apply and the models", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    openOn2375(fixture);
    // One task applies to a pull request: ←/→ keep it.
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    CHECK(dialogOf(fixture).form.task == 0);
    // The model field, past the worker, cycles the configured models.
    CHECK(fixture.dashboard.input("\t\t\x1b[C").empty());
    REQUIRE(dialogOf(fixture).setting == FormField::Model);
    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().model == "opus");
    CHECK(dispatched.front().task == "fix_ci");
}

TEST_CASE("a refusal the form cannot see is drawn", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.dispatchRefused(DispatchRefusal::AgentLimit);
    CHECK(fixture.screen().contains(
        std::format("dispatch refused: {}", refusalText(DispatchRefusal::AgentLimit))));
    // The next key clears it.
    CHECK(fixture.dashboard.input("j").empty());
    CHECK_FALSE(fixture.screen().contains("dispatch refused"));
}

TEST_CASE("a confirmation never reaches a form reopened in the same read", "[orchestrator][view][form]")
{
    // Esc and a key read together close the dialog; Enter on `+ new task` opens the `n` box, whose warning
    // the first Enter past its text confirms and no Enter of the same read gets past.
    for (auto const read: { "\x1b\td\rfix it\r\r"sv, "\x1b\x1b[Ad\rfix it\r\r"sv })
    {
        CAPTURE(read);
        auto fixture = DashboardFixture { hooksMissing() };
        openOn2375(fixture);
        CHECK(fixture.dashboard.input("\r").empty());
        REQUIRE(dialogOf(fixture).hooks == HooksWarning::Confirmed);

        CHECK(dispatchesIn(fixture.dashboard.input(read)).empty());
        CHECK_FALSE(isDialogOpen(fixture));
        auto const& box = fixture.dashboard.interaction().taskBox;
        REQUIRE(box.has_value());
        CHECK(box->form.session != "00000001-0000-4000-8000-000000000000"); // the second form
        CHECK(box->hooks == HooksWarning::Confirmed);
    }
}

TEST_CASE("a chord in the form dispatches nothing", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    openOn2375(fixture);
    // Esc and Enter within the Escape key's 50 ms read as Alt+Enter: the form is cancelled.
    CHECK(fixture.dashboard.input("\x1b\r").empty());
    CHECK_FALSE(isDialogOpen(fixture));

    openOn2375(fixture);
    CHECK(fixture.dashboard.input("\x01").empty()); // Ctrl+A
    CHECK(isDialogOpen(fixture));
}

TEST_CASE("a paste is text, never keys", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    // Every frame asks for bracketed paste.
    CHECK(fixture.dashboard.frame().contains(BracketedPasteOn));

    openOn2375(fixture);
    CHECK(fixture.dashboard.input("\x1b[200~a\nb\r\n\x1b[201~").empty());
    CHECK(dialogOf(fixture).form.note == "a b ");

    // On the list a paste is dropped: its Enter opens nothing; typing a filter, it extends the filter.
    auto list = DashboardFixture { World {} };
    CHECK(list.dashboard.input("\x1b[200~d\r\x1b[201~").empty());
    CHECK_FALSE(list.dashboard.interaction().taskBox.has_value());
    CHECK(list.dashboard.interaction() == Interaction {});
    CHECK(list.dashboard.input("/\x1b[200~std\nio\x1b[201~").empty());
    CHECK(list.dashboard.interaction().filter == "std io");
}

TEST_CASE("the form offers Enter only while Enter dispatches", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    openOn2375(fixture);
    CHECK(fixture.screen().contains("enter start · tab next · ←→ change"));
    CHECK(fixture.screen().contains("task      ‹ fix CI ›"));
    CHECK(fixture.dashboard.input("-").empty());
    CHECK_FALSE(fixture.screen().contains("enter start"));
    CHECK(fixture.screen().contains("tab next · ←→ change"));

    auto refused = DashboardFixture { limitReached() };
    openOn2375(refused);
    REQUIRE(isDialogOpen(refused));
    CHECK_FALSE(refused.screen().contains("enter start"));
}

TEST_CASE("n opens the box, and Enter dispatches what was typed as a topic",
          "[orchestrator][view][form][topic]")
{
    auto fixture = DashboardFixture { World {} };
    CHECK(fixture.dashboard.input("n").empty());
    REQUIRE(fixture.dashboard.interaction().taskBox.has_value());

    // A topic typed: `n` and `d` are text here, not keys.
    CHECK(fixture.dashboard.input("Investigate the flaky d-n test").empty());
    CHECK(fixture.screen().contains("agent/topic-investigate-the-flaky-d-n-test-…"));
    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(std::holds_alternative<TopicRef>(dispatched.front().item));
    CHECK(dispatched.front().topic == "Investigate the flaky d-n test");
    CHECK_FALSE(dispatched.front().note.has_value());
    CHECK(dispatched.front().url.empty());
    CHECK(dispatched.front().task == "topic");
    CHECK(dispatched.front().repo == "contour");
}

TEST_CASE("a topic starting with - is refused in the form", "[orchestrator][view][form][topic]")
{
    auto fixture = DashboardFixture { World {} };
    CHECK(fixture.dashboard.input("n-rf").empty());
    CHECK(fixture.screen().contains("a topic may not start with -"));
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(fixture.dashboard.interaction().taskBox.has_value());
}

TEST_CASE("a missing plugin is warned of and needs a second Enter", "[orchestrator][view][form][plugins]")
{
    auto fixture = DashboardFixture { World { .plugins = "claude/plugin-list-without.json" } };
    openOn2375(fixture);
    REQUIRE(dialogOf(fixture).hooks == HooksWarning::Shown);
    CHECK(fixture.screen().contains("lacks the plugin contour-workflows@contour-terminal"));

    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(fixture.screen().contains("enter starts it anyway"));
    CHECK(dispatchesIn(fixture.dashboard.input("\r")).size() == 1);
}

TEST_CASE("the palette installs the missing plugins it names", "[orchestrator][view][plugins]")
{
    auto fixture = DashboardFixture { World { .plugins = "claude/plugin-list-without.json" } };
    CHECK(fixture.screen().contains("lacks the plugin contour-workflows@contour-terminal · ^K installs it"));
    CHECK(fixture.screen().contains("plugins missing · ^K install"));
    // `I` is no key any more: it does nothing.
    CHECK(fixture.dashboard.input("I").empty());
    CHECK(fixture.dashboard.input("\x0binstall").empty());
    CHECK(fixture.screen().contains("install the missing plugins"));
    CHECK(fixture.dashboard.input("\r") == std::vector<UiAction> { InstallPluginsAction {} });
    CHECK_FALSE(fixture.dashboard.interaction().palette.has_value());

    SECTION("with nothing missing, the palette says so and installs nothing")
    {
        auto installed = DashboardFixture { World {} };
        CHECK(installed.dashboard.input("\x0binstall").empty());
        CHECK(installed.screen().contains("install the missing plugins · every plugin"));
        CHECK(installed.dashboard.input("\r").empty());
        CHECK(installed.screen().contains(
            "install the missing plugins · every plugin the tasks need is installed"));
    }
}

TEST_CASE("the worker refusals are said and told apart", "[orchestrator][view][form][workers]")
{
    CHECK_FALSE(refusalText(DispatchRefusal::UnknownWorker).empty());
    CHECK_FALSE(refusalText(DispatchRefusal::WorkerTakesNoProfile).empty());
    CHECK(refusalText(DispatchRefusal::UnknownWorker) != refusalText(DispatchRefusal::WorkerTakesNoProfile));
}

namespace
{
/// Two Claude workers, a and b; the pull-request task and the topic task both run on b.
constexpr auto TwoWorkersConfig = R"({ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "workers": [ { "id": "a", "agent": "claude" }, { "id": "b", "agent": "claude" } ],
  "repos": [ { "name": "contour", "path": "~/projects/contour", "forge": "github",
               "issues": [ { "source": "forge", "query": "assignee:@me" } ] } ],
  "tasks": [
    { "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
      "worker": "b", "prompt": "/contour-workflows:fix-ci {url}" },
    { "id": "plan", "key": "p", "label": "plan", "applies_to": "topic", "model": "opus",
      "worker": "b", "prompt": "/contour-workflows:writing-plans {topic}" } ] })"sv;
} // namespace

TEST_CASE("the forms open on the task's worker, else on the least busy one",
          "[orchestrator][view][form][workers]")
{
    SECTION("the task's worker runs nothing")
    {
        auto const engine = bootEngine(World { .configJson = std::string { TwoWorkersConfig } });
        REQUIRE(engine.config() != nullptr);
        auto const form = openDispatchForm(engine, "contour", ChangeRequestRef { 2375 }, "fix_ci", {});
        REQUIRE(form.has_value());
        CHECK(form->worker == 1);
        CHECK(workerOf(*form, engine)->id == "b");
        auto const topic = openTopicForm(engine, "contour", "tidy the docs", {});
        REQUIRE(topic.has_value());
        CHECK(topic->worker == 1);
    }

    SECTION("the task's worker runs an agent")
    {
        auto busy = storedRun(Running {}, "pr-2369-20261113-150847", ChangeRequestRef { 2369 });
        busy.worker = "b";
        auto const engine =
            bootEngine(World { .configJson = std::string { TwoWorkersConfig }, .stored = { busy } });
        REQUIRE(engine.config() != nullptr);
        auto const form = openDispatchForm(engine, "contour", ChangeRequestRef { 2375 }, "fix_ci", {});
        REQUIRE(form.has_value());
        CHECK(form->worker == 0);
        auto const topic = openTopicForm(engine, "contour", "tidy the docs", {});
        REQUIRE(topic.has_value());
        CHECK(topic->worker == 0);
    }
}

TEST_CASE("the worker setting cycles the workers, and the action carries it",
          "[orchestrator][view][form][workers]")
{
    auto const engine = bootEngine(World { .configJson = std::string { TwoWorkersConfig } });
    REQUIRE(engine.config() != nullptr);
    auto form = openDispatchForm(
        engine, "contour", ChangeRequestRef { 2375 }, "fix_ci", "123e4567-e89b-12d3-a456-426614174000");
    REQUIRE(form.has_value());
    CHECK(actionOf(*form, engine, "t1").worker == "b");
    // b, the last, wraps around to a; a step back returns to b.
    changeSetting(*form, FormField::Worker, 1, engine);
    CHECK(form->worker == 0);
    CHECK(actionOf(*form, engine, "t1").worker == "a");
    changeSetting(*form, FormField::Worker, -1, engine);
    CHECK(form->worker == 1);
    changeSetting(*form, FormField::Worker, 1, engine);
    REQUIRE(form->worker == 0);
    // A task brings its own worker back.
    changeSetting(*form, FormField::Task, 1, engine);
    CHECK(form->worker == 1);
    CHECK(previewOf(*form, engine).has_value());
}
