// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/Dashboard.hpp>
#include <orchestrator/view/DispatchForm.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/TaskBox.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;

namespace
{

/// @return The dispatches among @p actions.
[[nodiscard]] std::vector<DispatchAction> dispatchesIn(std::vector<UiAction> const& actions)
{
    auto found = std::vector<DispatchAction> {};
    for (auto const& action: actions)
        if (auto const* const dispatch = std::get_if<DispatchAction>(&action))
            found.push_back(*dispatch);
    return found;
}

/// @return A world of two repositories, contour and other, each listing the same pull requests.
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

/// @return The open box; it must be open.
[[nodiscard]] TaskBoxState const& boxOf(DashboardFixture const& fixture)
{
    REQUIRE(fixture.dashboard.interaction().taskBox.has_value());
    return *fixture.dashboard.interaction().taskBox;
}

/// Closes the open box with Esc.
void closeBox(DashboardFixture& fixture)
{
    CHECK(fixture.dashboard.input("\x1b").empty());
    CHECK(fixture.escapeTimesOut().empty());
    REQUIRE_FALSE(fixture.dashboard.interaction().taskBox.has_value());
}

} // namespace

TEST_CASE("n opens the box; ⏎ starts the text at once", "[orchestrator][view][taskbox]")
{
    auto fixture = DashboardFixture { World {} };
    CHECK(fixture.dashboard.input("n").empty());
    auto const& box = boxOf(fixture);
    CHECK(box.text.empty());
    CHECK(box.focus == TaskBoxField::Text);
    // Its session comes from the dashboard's source.
    CHECK(box.form.session == "00000001-0000-4000-8000-000000000000");
    CHECK(fixture.screen().contains("What should the agent do?"));

    // Letters are text here, not keys.
    CHECK(fixture.dashboard.input("investigate the flaky test").empty());
    CHECK(boxOf(fixture).text == "investigate the flaky test");
    CHECK(fixture.screen().contains("agent/topic-investigate-the-flaky-test-…"));
    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    auto const& action = dispatched.front();
    CHECK(std::holds_alternative<TopicRef>(action.item));
    CHECK(action.topic == "investigate the flaky test");
    CHECK(action.repo == "contour");
    auto const* const config = fixture.engine.config();
    auto const* const topicTask = config->firstTaskFor(AppliesTo::Topic);
    REQUIRE(topicTask != nullptr);
    CHECK(action.task == topicTask->id);
    CHECK(action.worker == defaultWorker(*config, fixture.engine.runs(), topicTask->id).id);
    CHECK(action.model == topicTask->model);
    CHECK(action.session == "00000001-0000-4000-8000-000000000000");
    CHECK(action.nearTab == DashboardFixture::Tab);
    CHECK_FALSE(action.note.has_value());
    CHECK(action.url.empty());
    CHECK_FALSE(fixture.dashboard.interaction().taskBox.has_value());
}

TEST_CASE("the n box: a blank text does not start", "[orchestrator][view][taskbox]")
{
    auto fixture = DashboardFixture { World {} };
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(fixture.dashboard.input("   ").empty());
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(boxOf(fixture).text == "   ");
    CHECK_FALSE(fixture.screen().contains("enter start"));
    // A topic starting with `-` would be read as an option: it does not start either.
    CHECK(fixture.dashboard.input("\x15-rf").empty()); // Ctrl+U clears the line
    CHECK(fixture.screen().contains("a topic may not start with -"));
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(boxOf(fixture).text == "-rf");
}

TEST_CASE("the n box takes the repository chip, else the selected row's", "[orchestrator][view][taskbox]")
{
    auto fixture = DashboardFixture { twoRepositories() };
    REQUIRE(fixture.engine.config() != nullptr);

    SECTION("the Agents view, nothing selected: the first")
    {
        CHECK(fixture.dashboard.input("n").empty());
        CHECK(boxOf(fixture).form.repo == "contour");
    }

    SECTION("the Work view, the chip on other: other")
    {
        CHECK(fixture.dashboard.input("w").empty());
        static_cast<void>(fixture.screen());
        CHECK(fixture.dashboard.input("\x1b[C\x1b[C").empty());
        REQUIRE(fixture.dashboard.interaction().work.repoChip == 2);
        static_cast<void>(fixture.screen());
        // Its first item selected: the line under its first heading.
        CHECK(fixture.dashboard.input("[Hj").empty());
        static_cast<void>(fixture.screen());
        CAPTURE(fixture.dashboard.interaction().work.selectedKey);
        REQUIRE(fixture.dashboard.interaction().work.selectedKey.starts_with("other/"));
        CHECK(fixture.dashboard.input("n").empty());
        CHECK(boxOf(fixture).form.repo == "other");
        closeBox(fixture);

        // The chip on, wrapping round to every repository, a row of other still selected: other.
        CHECK(fixture.dashboard.input("\x1b[C").empty());
        REQUIRE(fixture.dashboard.interaction().work.repoChip == 0);
        static_cast<void>(fixture.screen());
        CAPTURE(fixture.dashboard.interaction().work.selectedKey);
        REQUIRE(fixture.dashboard.interaction().work.selectedKey.starts_with("other/"));
        CHECK(fixture.dashboard.input("n").empty());
        CHECK(boxOf(fixture).form.repo == "other");
    }
}

TEST_CASE("run again on a finished topic run opens the box with its text", "[orchestrator][view][taskbox]")
{
    constexpr auto TopicRunId = "topic-tidy-the-docs-20261113-150912"sv;
    auto run = storedRun(Finished { .ended = Ended::Completed,
                                    .fate = Fate::Removed,
                                    .entries = {},
                                    .seen = Seen::Opened,
                                    .endedWallMs = DispatchWallMs },
                         TopicRunId,
                         TopicRef { .slug = "tidy-the-docs" },
                         "topic");
    run.repo = "other";
    run.repoPath = std::string { OtherPath };
    auto world = twoRepositories();
    world.stored = { run };
    auto fixture = DashboardFixture { world };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(TopicRunId).empty());
    CHECK(fixture.dashboard.input("\r").empty());
    auto const& box = boxOf(fixture);
    CHECK(box.text == "tidy the docs");
    CHECK(box.form.repo == "other");
    CHECK_FALSE(box.form.session.empty());
    CHECK_FALSE(fixture.dashboard.interaction().startDialog.has_value());
    // Nothing starts before it is confirmed; then it starts in other.
    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().topic == "tidy the docs");
    CHECK(dispatched.front().repo == "other");
}

TEST_CASE("+ new task in the Fleet opens the box", "[orchestrator][view][taskbox]")
{
    auto fixture = DashboardFixture { World {} };
    static_cast<void>(fixture.dashboard.frame());
    // `+ new task` is the only row, selected.
    CHECK(fixture.screen().contains("+ new task"));
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(boxOf(fixture).text.empty());
}

TEST_CASE("the n box: Tab reaches the repository, the worker and the model", "[orchestrator][view][taskbox]")
{
    auto fixture = DashboardFixture { twoRepositories() };
    CHECK(fixture.dashboard.input("ntidy the docs").empty());
    // ←/→ change nothing while the text has the keys.
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    CHECK(boxOf(fixture).form.repo == "contour");
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(boxOf(fixture).focus == TaskBoxField::Repo);
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    CHECK(boxOf(fixture).form.repo == "other");
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(boxOf(fixture).focus == TaskBoxField::Worker);
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(boxOf(fixture).focus == TaskBoxField::Model);
    auto const model = boxOf(fixture).form.model;
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    auto const stepped = boxOf(fixture).form.model;
    CHECK(stepped != model);
    // Typing still edits the text wherever the keys are; Tab returns to it, Shift+Tab goes back.
    CHECK(fixture.dashboard.input("!").empty());
    CHECK(boxOf(fixture).text == "tidy the docs!");
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(boxOf(fixture).focus == TaskBoxField::Text);
    CHECK(fixture.dashboard.input("\x1b[9;2u").empty()); // Shift+Tab
    CHECK(boxOf(fixture).focus == TaskBoxField::Model);

    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().repo == "other");
    CHECK(dispatched.front().model == fixture.engine.config()->models()[stepped]);
}

TEST_CASE("the n box: esc closes it; a paste is text", "[orchestrator][view][taskbox]")
{
    auto fixture = DashboardFixture { World {} };
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.input("\x1b[200~fix\r\nit\x1b[201~").empty());
    CHECK(boxOf(fixture).text == "fix it");
    closeBox(fixture);
    CHECK_FALSE(fixture.screen().contains("What should the agent do?"));
    // Esc and a key read together close it too.
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.input("\x1b\r").empty());
    CHECK_FALSE(fixture.dashboard.interaction().taskBox.has_value());
}

TEST_CASE("the n box: an incomplete setup asks for a second ⏎ of a later input",
          "[orchestrator][view][taskbox]")
{
    auto fixture = DashboardFixture { World { .hooks = "contour/setup-claude-absent.json" } };
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(boxOf(fixture).hooks == HooksWarning::Shown);
    CHECK(fixture.dashboard.input("tidy the docs").empty());
    CHECK(fixture.screen().contains("status hooks are not installed"));
    CHECK(fixture.screen().contains("enter confirms the warning"));
    // Two Enters in one read confirm the warning, and dispatch nothing.
    CHECK(fixture.dashboard.input("\r\r").empty());
    CHECK(boxOf(fixture).hooks == HooksWarning::Confirmed);
    CHECK(fixture.screen().contains("enter starts it anyway"));
    CHECK(dispatchesIn(fixture.dashboard.input("\r")).size() == 1);
}

TEST_CASE("the n box does not open unconfigured", "[orchestrator][view][taskbox]")
{
    auto fixture = DashboardFixture { World { .configJson = "{}" } };
    CHECK(fixture.dashboard.input("n").empty());
    CHECK_FALSE(fixture.dashboard.interaction().taskBox.has_value());
    CHECK(fixture.screen().contains("not configured"));
    CHECK(openTaskBox(fixture.engine, {}, {}, {}, {}).error() == StartRefusal::NotConfigured);
}
