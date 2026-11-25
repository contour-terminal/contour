// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/Dashboard.hpp>
#include <orchestrator/view/DispatchForm.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/StartDialog.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;
using core::tui::Size;

namespace
{

constexpr auto Wide = Size { .width = 120, .height = 40 };

/// @return The dispatches among @p actions.
[[nodiscard]] std::vector<DispatchAction> dispatchesIn(std::vector<UiAction> const& actions)
{
    auto found = std::vector<DispatchAction> {};
    for (auto const& action: actions)
        if (auto const* const dispatch = std::get_if<DispatchAction>(&action))
            found.push_back(*dispatch);
    return found;
}

/// @return A configuration of the Work world's repository with @p workers and @p tasks, as JSON arrays.
[[nodiscard]] std::string workConfig(std::string_view workers, std::string_view tasks)
{
    return std::format(R"({{ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "workers": {}, "repos": {}, "tasks": {} }})",
                       workers,
                       WorkRepository,
                       tasks);
}

/// The fix CI task, run by @p worker, with @p extra keys.
[[nodiscard]] std::string fixCiTask(std::string_view worker, std::string_view extra = {})
{
    return std::format(R"([ {{ "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr",
    "model": "sonnet", "worker": "{}", "prompt": "/contour-workflows:fix-ci {{url}}"{} }} ])",
                       worker,
                       extra);
}

/// @return The Work world on @p configJson, its runs @p stored.
[[nodiscard]] World workWorldWith(std::string configJson, std::vector<Run> stored)
{
    auto world = workWorld();
    world.configJson = std::move(configJson);
    world.stored = std::move(stored);
    return world;
}

/// Shows the Work view, its first item (#2375) selected, and presses ⏎ on it.
/// @return The open dialog.
[[nodiscard]] StartDialogState const& openOnFirst(DashboardFixture& fixture)
{
    CHECK(fixture.dashboard.input("w").empty());
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.input("\r").empty());
    REQUIRE(fixture.dashboard.interaction().startDialog.has_value());
    auto const& dialog = *fixture.dashboard.interaction().startDialog;
    REQUIRE(dialog.form.item == ItemRef { ChangeRequestRef { 2375 } });
    return dialog;
}

/// @return The id of the task the dialog's form starts.
[[nodiscard]] std::string taskOf(Engine const& engine, StartDialogState const& dialog)
{
    return engine.config()->tasks()[dialog.form.task].id;
}

} // namespace

TEST_CASE("Start dialog: ⏎ on #2375 suggests fix CI with its failing check", "[orchestrator][view][start]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    auto const& dialog = openOnFirst(fixture);
    CHECK(taskOf(fixture.engine, dialog) == "fix_ci");
    CHECK(dialog.reason == "pipeline fails: test-windows");
    CHECK(dialog.setting == FormField::Task);
    // Its session comes from the dashboard's source.
    CHECK(dialog.form.session == "00000001-0000-4000-8000-000000000000");
    auto const screen = fixture.screen();
    CHECK(screen.contains("▶ Start agent"));
    CHECK(screen.contains("‹ fix CI ›  pipeline fails: test-windows"));
    // Esc closes it, and asks nothing.
    CHECK(fixture.dashboard.input("\x1b").empty());
    CHECK(fixture.escapeTimesOut().empty());
    CHECK_FALSE(fixture.dashboard.interaction().startDialog.has_value());
}

TEST_CASE("Start dialog: the worker defaults by the busy rule and can be changed",
          "[orchestrator][view][start]")
{
    auto busy = storedRun(Running {}, "pr-2378-20261113-150847", ChangeRequestRef { 2378 });
    busy.worker = "a";
    auto fixture = DashboardFixture {
        workWorldWith(workConfig(R"([ { "id": "a", "agent": "claude" }, { "id": "b", "agent": "claude" } ])",
                                 fixCiTask("a")),
                      { busy }),
        Wide
    };
    REQUIRE(fixture.engine.config() != nullptr);
    auto const& dialog = openOnFirst(fixture);
    // a, the task's own, runs an agent: b runs none.
    CHECK(dialog.form.worker == 1);
    auto const screen = fixture.screen();
    CHECK(screen.contains(" a 1 "));
    CHECK(screen.contains(" b 0 "));

    // Tab reaches the worker; one step on wraps around to a.
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(fixture.dashboard.interaction().startDialog->setting == FormField::Worker);
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    CHECK(fixture.dashboard.interaction().startDialog->form.worker == 0);
    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().worker == "a");
    CHECK(dispatched.front().task == "fix_ci");
    CHECK(dispatched.front().item == ItemRef { ChangeRequestRef { 2375 } });
    CHECK(dispatched.front().repo == "contour");
    CHECK_FALSE(fixture.dashboard.interaction().startDialog.has_value());
}

TEST_CASE("Start dialog: the permission profile is its fifth setting", "[orchestrator][view][start]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    auto const& dialog = openOnFirst(fixture);
    auto const* const config = fixture.engine.config();
    auto const profiles = config->profiles();
    // It starts on the task's profile, here the default one.
    CHECK(profiles[dialog.form.permissions].name == config->profileFor("fix_ci").name);

    CHECK(fixture.dashboard.input("\t\t\t").empty());
    CHECK(fixture.dashboard.interaction().startDialog->setting == FormField::Permissions);
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    auto const stepped = fixture.dashboard.interaction().startDialog->form.permissions;
    CHECK(profiles[stepped].name != config->profileFor("fix_ci").name);
    // Then the note, and back to the task.
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(fixture.dashboard.interaction().startDialog->setting == FormField::Note);
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(fixture.dashboard.interaction().startDialog->setting == FormField::Task);

    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().permissions == profiles[stepped].name);
}

TEST_CASE("Start dialog: Copilot chosen for a task without permissions dispatches",
          "[orchestrator][view][start]")
{
    constexpr auto Workers =
        R"([ { "id": "claude", "agent": "claude" }, { "id": "copilot", "agent": "copilot" } ])"sv;

    SECTION("a task without permissions")
    {
        auto fixture = DashboardFixture { workWorldWith(workConfig(Workers, fixCiTask("claude")), {}), Wide };
        REQUIRE(fixture.engine.config() != nullptr);
        static_cast<void>(openOnFirst(fixture));
        CHECK(fixture.dashboard.input("\t\x1b[C").empty());
        REQUIRE(fixture.dashboard.interaction().startDialog->form.worker == 1);
        CHECK_FALSE(fixture.screen().contains("(no profile)"));
        // A worker that takes no profile offers none: Tab passes from the model to the note.
        CHECK(fixture.dashboard.input("\t\t").empty());
        CHECK(fixture.dashboard.interaction().startDialog->setting == FormField::Note);
        auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
        REQUIRE(dispatched.size() == 1);
        CHECK(dispatched.front().worker == "copilot");
    }

    SECTION("a task with permissions")
    {
        auto fixture = DashboardFixture {
            workWorldWith(workConfig(Workers, fixCiTask("claude", R"(, "permissions": "trusted")")), {}), Wide
        };
        REQUIRE(fixture.engine.config() != nullptr);
        static_cast<void>(openOnFirst(fixture));
        CHECK(fixture.screen().contains(" copilot 0 (no profile) "));
        CHECK(fixture.dashboard.input("\t\x1b[C").empty());
        auto const& dialog = *fixture.dashboard.interaction().startDialog;
        REQUIRE(dialog.form.worker == 1);
        CHECK(previewOf(dialog.form, fixture.engine).error() == DispatchRefusal::WorkerTakesNoProfile);
        CHECK(fixture.dashboard.input("\r").empty());
        CHECK(fixture.dashboard.interaction().startDialog.has_value());
        CHECK(fixture.screen().contains(refusalText(DispatchRefusal::WorkerTakesNoProfile).substr(0, 40)));
    }
}

TEST_CASE("Start dialog: an incomplete setup asks for a second ⏎", "[orchestrator][view][start]")
{
    auto world = workWorld();
    world.hooks = "contour/setup-hooks-missing.json";
    auto fixture = DashboardFixture { world, Wide };
    CHECK(openOnFirst(fixture).hooks == HooksWarning::Shown);
    auto const screen = fixture.screen();
    CHECK(screen.contains(" claude 1 (setup) "));
    CHECK(screen.contains("enter confirms the warning"));

    // Two Enters in one read confirm the warning, and dispatch nothing.
    CHECK(fixture.dashboard.input("\r\r").empty());
    REQUIRE(fixture.dashboard.interaction().startDialog.has_value());
    CHECK(fixture.dashboard.interaction().startDialog->hooks == HooksWarning::Confirmed);
    CHECK(fixture.screen().contains("enter starts it anyway"));
    // An Enter of a later input dispatches.
    CHECK(dispatchesIn(fixture.dashboard.input("\r")).size() == 1);
    CHECK_FALSE(fixture.dashboard.interaction().startDialog.has_value());
}

TEST_CASE("Start dialog: an item an agent works on shows so and does not dispatch",
          "[orchestrator][view][start]")
{
    auto world = workWorld();
    world.stored = { storedRun(Running {}, "pr-2375-20261113-150847", ChangeRequestRef { 2375 }) };
    auto fixture = DashboardFixture { world, Wide };
    static_cast<void>(openOnFirst(fixture));
    auto const screen = fixture.screen();
    CHECK(screen.contains("⬢ claude already works on this"));
    CHECK(screen.contains(refusalText(DispatchRefusal::AlreadyRunning)));
    CHECK_FALSE(screen.contains("enter start"));
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(fixture.dashboard.interaction().startDialog.has_value());
}

TEST_CASE("Start dialog: the list changing under it dispatches nothing stale", "[orchestrator][view][start]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    static_cast<void>(openOnFirst(fixture));
    // The next poll lists no #2375: it was merged.
    fixture.world.prs = { "github/prs-empty.json" };
    fixture.poll();
    REQUIRE(std::ranges::none_of(fixture.engine.changeRequests("contour"),
                                 [](ChangeRequest const& request) { return request.number == 2375; }));
    CHECK(fixture.dashboard.input("\r").empty());
    REQUIRE(fixture.dashboard.interaction().startDialog.has_value());
    CHECK(fixture.screen().contains(refusalText(DispatchRefusal::BaseUnknown)));
}

TEST_CASE("Start dialog: typing edits the note, Ctrl+P shows the prompt and the command",
          "[orchestrator][view][start]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    static_cast<void>(openOnFirst(fixture));
    // Typed text goes to the note wherever the keys are; a paste too, as text.
    CHECK(fixture.dashboard.input("keep it small").empty());
    CHECK(fixture.dashboard.input("\x1b[200~ please\r\x1b[201~").empty());
    CHECK(fixture.dashboard.interaction().startDialog->form.note == "keep it small please ");
    CHECK_FALSE(fixture.screen().contains("prompt    "));
    CHECK(fixture.dashboard.input("\x10").empty());
    CHECK(fixture.dashboard.interaction().startDialog->preview == DialogPreview::Full);
    auto const screen = fixture.screen();
    CHECK(screen.contains("prompt    "));
    CHECK(screen.contains("command   "));
    auto const dispatched = dispatchesIn(fixture.dashboard.input("\r"));
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().note == std::optional { "keep it small please "s });
}

TEST_CASE("Start dialog: it does not open on an item the engine no longer lists",
          "[orchestrator][view][start]")
{
    auto const engine = bootEngine(workWorld());
    auto const items = engine.workItems();
    REQUIRE_FALSE(items.empty());
    auto gone = items.front();
    gone.changeRequest->number = 9999;
    gone.item = ChangeRequestRef { 9999 };
    CHECK(openStartDialog(engine, gone, {}, {}).error() == StartRefusal::NotListed);
    CHECK(startRefusalNotice(StartRefusal::NotListed, "#9999") == "#9999 is not listed any more");
    // A task asked for, while it applies, is the one it opens on.
    auto const asked = openStartDialog(engine, items.front(), "fix_ci", {});
    REQUIRE(asked.has_value());
    CHECK(engine.config()->tasks()[asked->form.task].id == "fix_ci");
}
