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
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;
using core::tui::Size;

namespace
{

constexpr auto Wide = Size { .width = 120, .height = 40 };
constexpr auto Medium = Size { .width = 80, .height = 24 };
constexpr auto Narrow = Size { .width = 50, .height = 20 };

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

} // namespace

TEST_CASE("dispatch form goldens", "[orchestrator][view][form]")
{
    struct Scene
    {
        std::string_view name;
        World world;
        ItemRef item;
        std::string_view task;
        Size size;
    };
    auto const scenes = std::vector<Scene> {
        Scene {
            .name = "pr", .world = {}, .item = ChangeRequestRef { 2375 }, .task = "fix_ci", .size = Wide },
        Scene {
            .name = "pr", .world = {}, .item = ChangeRequestRef { 2375 }, .task = "fix_ci", .size = Medium },
        Scene {
            .name = "pr", .world = {}, .item = ChangeRequestRef { 2375 }, .task = "fix_ci", .size = Narrow },
        Scene { .name = "issue", .world = {}, .item = IssueRef { 2356 }, .task = "issue", .size = Wide },
        Scene { .name = "hooks-missing",
                .world = hooksMissing(),
                .item = ChangeRequestRef { 2375 },
                .task = "fix_ci",
                .size = Wide },
        Scene { .name = "hooks-missing",
                .world = hooksMissing(),
                .item = ChangeRequestRef { 2375 },
                .task = "fix_ci",
                .size = Narrow },
        Scene { .name = "limit",
                .world = limitReached(),
                .item = ChangeRequestRef { 2375 },
                .task = "fix_ci",
                .size = Medium },
    };
    for (auto const& scene: scenes)
    {
        auto const name = std::format("launcher-{}-{}x{}", scene.name, scene.size.width, scene.size.height);
        SECTION(name)
        {
            auto fixture = DashboardFixture { scene.world, scene.size };
            fixture.dashboard.openLauncherOn("contour", scene.item, scene.task);
            REQUIRE(fixture.dashboard.launcher().has_value());
            checkGolden(name, fixture.screen());
        }
    }
}

TEST_CASE("the form's session comes from the dashboard's source", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    REQUIRE(fixture.dashboard.launcher().has_value());
    CHECK(fixture.dashboard.launcher()->form->session == "00000001-0000-4000-8000-000000000000");
    CHECK(fixture.ids.taken == 1);
}

TEST_CASE("a note starting with - is refused in the form", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    CHECK(fixture.dashboard.input("\t\t\t\t-rf").empty());
    REQUIRE(fixture.dashboard.launcher()->setting == FormField::Note);
    CHECK(fixture.dashboard.launcher()->form->note == "-rf");
    CHECK(fixture.screen().contains("a note may not start with -"));

    // Enter does nothing while it does, and the form stays open.
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(fixture.dashboard.launcher().has_value());

    // Once it no longer starts with one, Enter dispatches it.
    CHECK(fixture.dashboard
              .input("\x7f\x7f\x7f"
                     "keep it small")
              .empty());
    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().note == std::optional<std::string> { "keep it small" });
    CHECK_FALSE(fixture.dashboard.launcher().has_value());
}

TEST_CASE("the command line equals the template substituted", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    auto const& form = *fixture.dashboard.launcher()->form;
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
    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    REQUIRE(fixture.dashboard.launcher()->hooks == HooksWarning::Shown);
    CHECK(fixture.screen().contains("status hooks are not installed"));

    CHECK(fixture.dashboard.input("\r").empty());
    REQUIRE(fixture.dashboard.launcher().has_value());
    CHECK(fixture.dashboard.launcher()->hooks == HooksWarning::Confirmed);
    CHECK(fixture.screen().contains("enter starts it anyway"));

    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().nearTab == DashboardFixture::Tab);
}

TEST_CASE("two Enters in one read do not confirm the hooks warning", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { hooksMissing() };
    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");

    // A paste, or a key held down: both Enters come in one read, and the second asks nothing.
    CHECK(fixture.dashboard.input("\r\r").empty());
    CHECK(fixture.dashboard.input("\r").size() == 1); // a later read dispatches
    CHECK_FALSE(fixture.dashboard.launcher().has_value());
}

TEST_CASE("esc cancels the form and asks nothing", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    CHECK(fixture.dashboard.input("\x1b").empty());
    fixture.escapeTimesOut();
    CHECK_FALSE(fixture.dashboard.launcher().has_value());
    CHECK_FALSE(fixture.screen().contains("New agent"));
}

TEST_CASE("the form cycles the tasks that apply and the models", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    // One task applies to a pull request: ←/→ keep it.
    CHECK(fixture.dashboard.input("\t\x1b[C").empty());
    CHECK(fixture.dashboard.launcher()->form->task == 0);
    // The model field cycles the configured models.
    CHECK(fixture.dashboard.input("\t\x1b[C").empty());
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
    // Esc and a key read together close the form; `d` opens another, whose warning the first Enter confirms
    // and no Enter of the same read gets past.
    for (auto const read: { "\x1b\td\r\r"sv, "\x1b\x1b[Ad\r\r"sv })
    {
        CAPTURE(read);
        auto fixture = DashboardFixture { hooksMissing() };
        fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
        CHECK(fixture.dashboard.input("\r").empty());
        REQUIRE(fixture.dashboard.launcher()->hooks == HooksWarning::Confirmed);

        CHECK(dispatchesIn(fixture.dashboard.input(read)).empty());
        REQUIRE(fixture.dashboard.launcher().has_value());
        CHECK(fixture.dashboard.launcher()->form->session
              != "00000001-0000-4000-8000-000000000000"); // the second form
        CHECK(fixture.dashboard.launcher()->hooks == HooksWarning::Confirmed);
    }
}

TEST_CASE("a chord in the form dispatches nothing", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    // Esc and Enter within the Escape key's 50 ms read as Alt+Enter: the form is cancelled.
    CHECK(fixture.dashboard.input("\x1b\r").empty());
    CHECK_FALSE(fixture.dashboard.launcher().has_value());

    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    CHECK(fixture.dashboard.input("\x01").empty()); // Ctrl+A
    CHECK(fixture.dashboard.launcher().has_value());
}

TEST_CASE("a paste is text, never keys", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    // Every frame asks for bracketed paste.
    CHECK(fixture.dashboard.frame().contains(BracketedPasteOn));

    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    CHECK(fixture.dashboard.input("\t\t\t\t").empty());
    CHECK(fixture.dashboard.input("\x1b[200~a\nb\r\n\x1b[201~").empty());
    REQUIRE(fixture.dashboard.launcher().has_value());
    CHECK(fixture.dashboard.launcher()->form->note == "a b ");

    // Back on the list, a paste goes into the query, as one line, and confirms nothing.
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(fixture.dashboard.input("\x1b[200~fla\rky\x1b[201~").empty());
    REQUIRE(fixture.dashboard.launcher().has_value());
    CHECK(fixture.dashboard.launcher()->query == "#2375fla ky");

    // On the list a paste is dropped: its Enter opens nothing; typing a filter, it extends the filter.
    auto list = DashboardFixture { World {} };
    CHECK(list.dashboard.input("\x1b[200~d\r\x1b[201~").empty());
    CHECK_FALSE(list.dashboard.launcher().has_value());
    CHECK(list.dashboard.interaction() == Interaction {});
    CHECK(list.dashboard.input("/\x1b[200~std\nio\x1b[201~").empty());
    CHECK(list.dashboard.interaction().filter == "std io");
}

TEST_CASE("the form offers Enter only while Enter dispatches", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    CHECK(fixture.screen().contains("enter start · ↑↓ choose · ←→ repo"));
    CHECK(fixture.screen().contains("task ‹ fix CI ›"));
    CHECK(fixture.dashboard.input("\t\t\t\t-").empty());
    CHECK_FALSE(fixture.screen().contains("enter start"));
    CHECK(fixture.screen().contains("↑↓ choose · tab settings"));

    auto refused = DashboardFixture { limitReached() };
    refused.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    CHECK_FALSE(refused.screen().contains("enter start"));
}

TEST_CASE("n opens the launcher, and Enter on the topic row dispatches what was typed",
          "[orchestrator][view][launcher][topic]")
{
    auto fixture = DashboardFixture { World {} };
    CHECK(fixture.dashboard.input("n").empty());
    REQUIRE(fixture.dashboard.launcher().has_value());
    CHECK(fixture.dashboard.launcher()->query.empty());
    CHECK(fixture.screen().contains("type to filter, or describe a topic"));

    // A topic typed: `n` and `d` are text here, not keys.
    CHECK(fixture.dashboard.input("Investigate the flaky d-n test").empty());
    REQUIRE(fixture.dashboard.launcher()->form.has_value());
    CHECK(std::holds_alternative<TopicRef>(fixture.dashboard.launcher()->form->item));
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
    CHECK(fixture.dashboard.launcher().has_value());
}

TEST_CASE("a finished topic run's form opens on its text", "[orchestrator][view][form][topic]")
{
    auto fixture = DashboardFixture { World {} };
    auto interaction = Interaction {};
    openLauncherFor(interaction, fixture.engine, "contour", TopicRef { .slug = "fix-it" }, "topic", "Fix it");
    REQUIRE(interaction.launcher.has_value());
    REQUIRE(interaction.launcher->form.has_value());
    CHECK(interaction.launcher->query == "Fix it");
    CHECK(interaction.launcher->form->topic == "Fix it");
    CHECK(interaction.launcher->form->repo == "contour");
    CHECK(std::holds_alternative<TopicRef>(interaction.launcher->form->item));
}

TEST_CASE("the launcher browses with nothing typed", "[orchestrator][view][launcher]")
{
    for (auto const size: { Wide, Medium })
    {
        auto const name = std::format("launcher-empty-{}x{}", size.width, size.height);
        SECTION(name)
        {
            auto fixture = DashboardFixture { World {}, size };
            CHECK(fixture.dashboard.input("n").empty());
            REQUIRE(fixture.dashboard.launcher().has_value());
            auto const screen = fixture.screen();
            CHECK(screen.contains("NEEDS ATTENTION"));
            CHECK(screen.contains("ISSUES WITHOUT A PR"));
            checkGolden(name, screen);
        }
    }
}

TEST_CASE("the launcher names each row's repository while it lists several", "[orchestrator][view][launcher]")
{
    auto const world = World {
        .repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github",
                        "issues": [ { "source": "forge", "query": "assignee:@me" } ] },
                      { "name": "other", "path": "~/projects/other", "forge": "github",
                        "issues": [ { "source": "forge", "query": "assignee:@me" } ] } ])",
        .repoPaths = { std::string { ContourPath }, std::string { OtherPath } },
    };
    auto fixture = DashboardFixture { world, core::tui::Size { .width = 160, .height = 40 } };
    CHECK(fixture.dashboard.input("n").empty());
    auto const every = fixture.screen();
    CHECK(every.contains("repo  all  contour  other    show  all  needs attention"));
    CHECK(every.contains("other  ✗"));
    checkGolden("launcher-repos-160x40", every);

    // Narrowed to one, the rows no longer name it.
    CHECK(fixture.dashboard.input("\x1b[C\x1b[C").empty());
    auto const narrowed = fixture.screen();
    CHECK_FALSE(narrowed.contains("contour  ✗"));
    checkGolden("launcher-repos-narrowed-160x40", narrowed);
}

TEST_CASE("topic form goldens", "[orchestrator][view][form][topic]")
{
    for (auto const size: { Wide, Medium })
    {
        auto const name = std::format("launcher-topic-{}x{}", size.width, size.height);
        SECTION(name)
        {
            auto fixture = DashboardFixture { World {}, size };
            CHECK(fixture.dashboard.input("nInvestigate why the shell integration test hangs on CI").empty());
            REQUIRE(fixture.dashboard.launcher().has_value());
            checkGolden(name, fixture.screen());
        }
    }
}

TEST_CASE("a missing plugin is warned of and needs a second Enter", "[orchestrator][view][form][plugins]")
{
    auto fixture = DashboardFixture { World { .plugins = "claude/plugin-list-without.json" } };
    fixture.dashboard.openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    REQUIRE(fixture.dashboard.launcher()->hooks == HooksWarning::Shown);
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
