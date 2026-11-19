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
        Scene { .name = "dispatch-pr",
                .world = {},
                .item = ChangeRequestRef { 2375 },
                .task = "fix_ci",
                .size = Wide },
        Scene { .name = "dispatch-pr",
                .world = {},
                .item = ChangeRequestRef { 2375 },
                .task = "fix_ci",
                .size = Medium },
        Scene { .name = "dispatch-pr",
                .world = {},
                .item = ChangeRequestRef { 2375 },
                .task = "fix_ci",
                .size = Narrow },
        Scene {
            .name = "dispatch-issue", .world = {}, .item = IssueRef { 2356 }, .task = "issue", .size = Wide },
        Scene { .name = "dispatch-hooks-missing",
                .world = hooksMissing(),
                .item = ChangeRequestRef { 2375 },
                .task = "fix_ci",
                .size = Wide },
        Scene { .name = "dispatch-hooks-missing",
                .world = hooksMissing(),
                .item = ChangeRequestRef { 2375 },
                .task = "fix_ci",
                .size = Narrow },
        Scene { .name = "dispatch-limit",
                .world = limitReached(),
                .item = ChangeRequestRef { 2375 },
                .task = "fix_ci",
                .size = Medium },
    };
    for (auto const& scene: scenes)
    {
        auto const name = std::format("{}-{}x{}", scene.name, scene.size.width, scene.size.height);
        SECTION(name)
        {
            auto fixture = DashboardFixture { scene.world, scene.size };
            fixture.dashboard.openForm("contour", scene.item, scene.task);
            REQUIRE(fixture.dashboard.form().has_value());
            checkGolden(name, fixture.screen());
        }
    }
}

TEST_CASE("the form's session comes from the dashboard's source", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");
    REQUIRE(fixture.dashboard.form().has_value());
    CHECK(fixture.dashboard.form()->session == "00000001-0000-4000-8000-000000000000");
    CHECK(fixture.ids.taken == 1);
}

TEST_CASE("a note starting with - is refused in the form", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");
    CHECK(fixture.dashboard.input("\t\t-rf").empty());
    REQUIRE(fixture.dashboard.form()->focus == FormField::Note);
    CHECK(fixture.dashboard.form()->note == "-rf");
    CHECK(fixture.screen().contains("a note may not start with -"));

    // Enter does nothing while it does, and the form stays open.
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(fixture.dashboard.form().has_value());

    // Once it no longer starts with one, Enter dispatches it.
    CHECK(fixture.dashboard
              .input("\x7f\x7f\x7f"
                     "keep it small")
              .empty());
    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().note == std::optional<std::string> { "keep it small" });
    CHECK_FALSE(fixture.dashboard.form().has_value());
}

TEST_CASE("the command line equals the template substituted", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");
    auto const& form = *fixture.dashboard.form();
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

    // The form shows that argv as one line a shell reads back word for word.
    auto const screen = fixture.screen();
    auto const line = shellQuoted(*argv);
    CHECK(line.starts_with("claude --model sonnet --session-id "));
    CHECK(screen.contains("claude --model sonnet --session-id"));
    CHECK(screen.contains("'/contour-workflows:fix-ci"));
}

TEST_CASE("hooks missing needs a second Enter", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { hooksMissing() };
    fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");
    REQUIRE(fixture.dashboard.form()->hooks == HooksWarning::Shown);
    CHECK(fixture.screen().contains("status hooks are not installed"));

    CHECK(fixture.dashboard.input("\r").empty());
    REQUIRE(fixture.dashboard.form().has_value());
    CHECK(fixture.dashboard.form()->hooks == HooksWarning::Confirmed);
    CHECK(fixture.screen().contains("enter again to dispatch"));

    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().near == DashboardFixture::Tab);
}

TEST_CASE("two Enters in one read do not confirm the hooks warning", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { hooksMissing() };
    fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");

    // A paste, or a key held down: both Enters come in one read, and the second asks nothing.
    CHECK(fixture.dashboard.input("\r\r").empty());
    CHECK(fixture.dashboard.input("\r").size() == 1); // a later read dispatches
    CHECK_FALSE(fixture.dashboard.form().has_value());
}

TEST_CASE("esc cancels the form and asks nothing", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");
    CHECK(fixture.dashboard.input("\x1b").empty());
    fixture.escapeTimesOut();
    CHECK_FALSE(fixture.dashboard.form().has_value());
    CHECK_FALSE(fixture.screen().contains("Dispatch an agent"));
}

TEST_CASE("the form cycles the tasks that apply and the models", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");
    // One task applies to a pull request: ←/→ keep it.
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    CHECK(fixture.dashboard.form()->task == 0);
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
        fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");
        CHECK(fixture.dashboard.input("\r").empty());
        REQUIRE(fixture.dashboard.form()->hooks == HooksWarning::Confirmed);

        CHECK(dispatchesIn(fixture.dashboard.input(read)).empty());
        REQUIRE(fixture.dashboard.form().has_value());
        CHECK(fixture.dashboard.form()->session != "00000001-0000-4000-8000-000000000000"); // the second form
        CHECK(fixture.dashboard.form()->hooks == HooksWarning::Confirmed);
    }
}

TEST_CASE("a chord in the form dispatches nothing", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");
    // Esc and Enter within the Escape key's 50 ms read as Alt+Enter: the form is cancelled.
    CHECK(fixture.dashboard.input("\x1b\r").empty());
    CHECK_FALSE(fixture.dashboard.form().has_value());

    fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");
    CHECK(fixture.dashboard.input("\x01").empty()); // Ctrl+A
    CHECK(fixture.dashboard.form().has_value());
}

TEST_CASE("a paste is text, never keys", "[orchestrator][view][form]")
{
    auto fixture = DashboardFixture { World {} };
    // Every frame asks for bracketed paste.
    CHECK(fixture.dashboard.frame().contains(BracketedPasteOn));

    fixture.dashboard.openForm("contour", ChangeRequestRef { 2375 }, "fix_ci");
    CHECK(fixture.dashboard.input("\t\t").empty());
    CHECK(fixture.dashboard.input("\x1b[200~a\nb\r\n\x1b[201~").empty());
    CHECK(fixture.dashboard.form()->note == "a b ");
    REQUIRE(fixture.dashboard.form().has_value());

    // With the task focused, a paste is dropped.
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(fixture.dashboard.input("\x1b[200~x\r\x1b[201~").empty());
    CHECK(fixture.dashboard.form()->note == "a b ");

    // On the list a paste is dropped: its Enter opens nothing; typing a filter, it extends the filter.
    auto list = DashboardFixture { World {} };
    CHECK(list.dashboard.input("\x1b[200~d\r\x1b[201~").empty());
    CHECK_FALSE(list.dashboard.form().has_value());
    CHECK(list.dashboard.interaction() == Interaction {});
    CHECK(list.dashboard.input("/\x1b[200~std\nio\x1b[201~").empty());
    CHECK(list.dashboard.interaction().filter == "std io");
}
