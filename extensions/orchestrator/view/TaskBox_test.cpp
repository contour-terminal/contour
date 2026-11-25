// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <format>
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

/// Presses Esc on its own: the key the terminal's Escape stands for, once its wait is over.
void pressEsc(DashboardFixture& fixture)
{
    CHECK(fixture.dashboard.input("\x1b").empty());
    CHECK(fixture.escapeTimesOut().empty());
}

/// Closes the open box, its text typed: Esc leaves the text, a second Esc closes.
void closeBox(DashboardFixture& fixture)
{
    REQUIRE(boxOf(fixture).mode == FieldMode::Typing);
    pressEsc(fixture);
    pressEsc(fixture);
    REQUIRE_FALSE(fixture.dashboard.interaction().taskBox.has_value());
}

/// @return A world of two workers, for the busy rule's topic task.
[[nodiscard]] World twoWorkers()
{
    auto world = World {};
    world.configJson = R"({ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "workers": [ { "id": "a", "agent": "claude" }, { "id": "b", "agent": "claude" } ],
  "repos": [ { "name": "contour", "path": "~/projects/contour", "forge": "github",
               "issues": [ { "source": "forge", "query": "assignee:@me" } ] } ],
  "tasks": [ { "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
               "prompt": "/contour-workflows:fix-ci {url}" } ] })";
    return world;
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
    // A letter types nothing off the text; Tab returns to it, typing, Shift+Tab goes back.
    CHECK(fixture.dashboard.input("!").empty());
    CHECK(boxOf(fixture).text == "tidy the docs");
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(boxOf(fixture).focus == TaskBoxField::Text);
    CHECK(boxOf(fixture).mode == FieldMode::Typing);
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
    // Esc and a key read together leave the text, as Esc does; read so again, they close it.
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.input("\x1b\r").empty());
    CHECK(boxOf(fixture).mode == FieldMode::Navigating);
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

TEST_CASE("the n box: a busy worker gives way only to one the topic task's profile can go to",
          "[orchestrator][view][taskbox]")
{
    auto world = World {
        .configJson = std::format(R"({{ "worktree_root": "~/worktrees", "models": ["opus"],
  "workers": [ {{ "id": "claude", "agent": "claude" }}, {{ "id": "copilot", "agent": "copilot" }} ],
  "repos": {},
  "tasks": [
    {{ "id": "plan", "key": "p", "label": "plan", "applies_to": "topic", "model": "opus", "worker": "claude",
       "permissions": "trusted", "prompt": "/plan {{topic}}" }} ] }})",
                                  OneRepository),
    };
    // A run with no worker of its own counts for `claude`, which is then busy.
    world.stored = { storedRun(
        Running {}, "topic-busy-20261113-150847", TopicRef { .slug = "busy" }, "plan") };
    auto fixture = DashboardFixture { world };
    REQUIRE(fixture.engine.config() != nullptr);
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.input("plan the release").empty());
    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    // Copilot is idle but takes no profile: the busy task's own worker stays, and ⏎ started it.
    CHECK(dispatched.front().worker == "claude");
}

TEST_CASE("n box: opens typing; esc goes to the settings; a second esc closes",
          "[orchestrator][view][taskbox]")
{
    auto fixture = DashboardFixture { twoWorkers() };
    REQUIRE(fixture.engine.config() != nullptr);
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(boxOf(fixture).focus == TaskBoxField::Text);
    CHECK(boxOf(fixture).mode == FieldMode::Typing);
    CHECK(fixture.screen().contains("esc settings"));

    // The vim letters type.
    CHECK(fixture.dashboard.input("fix jkhlqgGi").empty());
    CHECK(boxOf(fixture).text == "fix jkhlqgGi");
    CHECK(fixture.screen().contains("enter start · esc settings"));

    // Esc goes to Repo, navigating; j goes to Worker, and l steps the worker as → does.
    pressEsc(fixture);
    CHECK(boxOf(fixture).focus == TaskBoxField::Repo);
    CHECK(boxOf(fixture).mode == FieldMode::Navigating);
    CHECK(fixture.screen().contains("enter start · ↑↓ move · ←→ change · i text · esc close"));
    CHECK(fixture.dashboard.input("j").empty());
    CHECK(boxOf(fixture).focus == TaskBoxField::Worker);
    auto const first = boxOf(fixture).form.worker;
    CHECK(fixture.dashboard.input("l").empty());
    auto const next = boxOf(fixture).form.worker;
    CHECK(next != first);
    CHECK(fixture.dashboard.input("\x1b[D").empty());
    CHECK(boxOf(fixture).form.worker == first);
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    CHECK(boxOf(fixture).form.worker == next);

    // k k reaches the text, which it only selects: x types nothing there.
    CHECK(fixture.dashboard.input("kk").empty());
    CHECK(boxOf(fixture).focus == TaskBoxField::Text);
    CHECK(boxOf(fixture).mode == FieldMode::Navigating);
    CHECK(fixture.screen().contains("⏎ text · ↑↓ move · ←→ change · i text · esc close"));
    CHECK(fixture.dashboard.input("x").empty());
    CHECK(boxOf(fixture).text == "fix jkhlqgGi");

    // ⏎ on the text types it; Esc goes back to Repo.
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(boxOf(fixture).mode == FieldMode::Typing);
    CHECK(boxOf(fixture).focus == TaskBoxField::Text);
    pressEsc(fixture);
    CHECK(boxOf(fixture).focus == TaskBoxField::Repo);

    // A paste while navigating lands in the text.
    CHECK(fixture.dashboard.input("\x1b[200~!\x1b[201~").empty());
    CHECK(boxOf(fixture).text == "fix jkhlqgGi!");
    CHECK(boxOf(fixture).mode == FieldMode::Navigating);

    // i goes to the text, typing; Esc then q closes.
    CHECK(fixture.dashboard.input("i").empty());
    CHECK(boxOf(fixture).focus == TaskBoxField::Text);
    CHECK(boxOf(fixture).mode == FieldMode::Typing);
    pressEsc(fixture);
    CHECK(fixture.dashboard.input("q").empty());
    CHECK_FALSE(fixture.dashboard.interaction().taskBox.has_value());

    // Typing and ⏎ at once still start: the one-step flow.
    CHECK(fixture.dashboard.input("n").empty());
    auto const dispatched = dispatchesIn(fixture.dashboard.input("tidy the docs\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().topic == "tidy the docs");
}

TEST_CASE("n box: the line-editing keys edit the text, and on the settings do nothing",
          "[orchestrator][view][taskbox]")
{
    auto fixture = DashboardFixture { twoWorkers() };
    REQUIRE(fixture.engine.config() != nullptr);
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.input("fix the flaky test").empty());
    CHECK(fixture.dashboard.input("\x1b\x7f").empty()); // Alt+Backspace drops a word
    CHECK(boxOf(fixture).text == "fix the flaky ");
    CHECK(fixture.dashboard.input("\x17").empty()); // Ctrl+W drops back to the last blank
    CHECK(boxOf(fixture).text == "fix the ");
    CHECK(boxOf(fixture).mode == FieldMode::Typing);

    // On the settings there is no text to edit: the box stays open, its text whole.
    pressEsc(fixture);
    REQUIRE(boxOf(fixture).mode == FieldMode::Navigating);
    for (auto const* const edit: { "\x1b\x7f", "\x17", "\x7f", "\x15" })
    {
        CAPTURE(edit);
        CHECK(fixture.dashboard.input(edit).empty());
        CHECK(boxOf(fixture).text == "fix the ");
        CHECK(boxOf(fixture).mode == FieldMode::Navigating);
    }
}

TEST_CASE("n box: with close rebound away from esc, Esc still closes it on the settings",
          "[orchestrator][view][taskbox]")
{
    auto world = twoWorkers();
    REQUIRE(world.configJson.has_value());
    world.configJson->insert(1, R"( "keys": { "close": ["q"] },)");
    auto fixture = DashboardFixture { world };
    REQUIRE(fixture.engine.config() != nullptr);
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.input("tidy").empty());
    pressEsc(fixture);
    REQUIRE(boxOf(fixture).mode == FieldMode::Navigating);
    pressEsc(fixture);
    CHECK_FALSE(fixture.dashboard.interaction().taskBox.has_value());

    // A key with Alt reads as that Esc.
    CHECK(fixture.dashboard.input("n").empty());
    pressEsc(fixture);
    REQUIRE(boxOf(fixture).mode == FieldMode::Navigating);
    CHECK(fixture.dashboard.input("\x1bj").empty());
    CHECK_FALSE(fixture.dashboard.interaction().taskBox.has_value());
}
