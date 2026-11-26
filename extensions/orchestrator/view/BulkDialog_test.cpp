// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/BulkDialog.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;

namespace
{

/// @return The Work world's items in @p group, as the board lists them.
[[nodiscard]] std::vector<WorkItem> itemsIn(Engine const& engine, WorkGroup group)
{
    return engine.workItems() | std::views::filter([&](WorkItem const& item) { return item.group == group; })
           | std::ranges::to<std::vector>();
}

} // namespace

TEST_CASE("Bulk dialog: opens on a group with the suggested tasks and counts what it starts",
          "[orchestrator][view][bulk]")
{
    auto const engine = bootEngine(workWorld());
    auto const needsYou = itemsIn(engine, WorkGroup::NeedsYou);
    // #2375 fails, #2376 has a run, #2377 conflicts.
    REQUIRE(needsYou.size() == 3);
    auto const dialog = openBulkDialog(engine, WorkGroup::NeedsYou, needsYou);
    REQUIRE(dialog.has_value());
    CHECK(dialog->choice == 0);
    CHECK(bulkCount(engine, *dialog) == 2);
    auto const lines = bulkLines(engine, *dialog);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front() == "Start the suggested task on 2 merge requests");
}

TEST_CASE("Bulk dialog: one task for all counts the items it applies to", "[orchestrator][view][bulk]")
{
    auto const engine = bootEngine(workWorld());
    auto dialog = *openBulkDialog(engine, WorkGroup::NeedsYou, itemsIn(engine, WorkGroup::NeedsYou));
    auto const fixCi = bulkChoices(engine, dialog);
    // The suggested task first, then each task that applies to some item of the group.
    REQUIRE(fixCi.size() >= 2);
    CHECK(fixCi.front().empty());
    dialog.choice = static_cast<std::size_t>(std::ranges::find(fixCi, "fix_ci") - fixCi.begin());
    REQUIRE(dialog.choice < fixCi.size());
    CHECK(bulkCount(engine, dialog) == 2);
    CHECK(bulkLines(engine, dialog).front() == "Start fix CI on 2 merge requests");
    CHECK_FALSE(std::ranges::contains(fixCi, "issue"));
}

TEST_CASE("Bulk dialog: ←/→ step the task, ⏎ starts, esc closes", "[orchestrator][view][bulk]")
{
    auto const engine = bootEngine(workWorld());
    auto interaction = Interaction {};
    interaction.bulk = openBulkDialog(engine, WorkGroup::NeedsYou, itemsIn(engine, WorkGroup::NeedsYou));
    interaction.overlay = Overlay::Bulk;
    auto const context = KeyContext { .engine = &engine };
    auto const right = core::tui::KeyEvent { .key = core::tui::KeyCode::Right };
    CHECK_FALSE(applyResolvedKey(right, interaction, context).has_value());
    REQUIRE(interaction.bulk.has_value());
    CHECK(interaction.bulk->choice == 1);
    auto const choices = bulkChoices(engine, *interaction.bulk);

    auto const started =
        applyResolvedKey(core::tui::KeyEvent { .key = core::tui::KeyCode::Enter }, interaction, context);
    REQUIRE(started.has_value());
    auto const* const run = std::get_if<RunOnItemsAction>(&*started);
    REQUIRE(run != nullptr);
    CHECK(run->task == choices[1]);
    CHECK(run->items == std::vector<std::string> { "contour/!2375", "contour/!2377" });
    CHECK_FALSE(interaction.bulk.has_value());
    CHECK(interaction.overlay == Overlay::None);

    interaction.bulk = openBulkDialog(engine, WorkGroup::NeedsYou, itemsIn(engine, WorkGroup::NeedsYou));
    interaction.overlay = Overlay::Bulk;
    CHECK_FALSE(
        applyResolvedKey(core::tui::KeyEvent { .key = core::tui::KeyCode::Escape }, interaction, context)
            .has_value());
    CHECK_FALSE(interaction.bulk.has_value());
    CHECK(interaction.overlay == Overlay::None);
}

TEST_CASE("Bulk dialog: a group whose items all have runs does not open", "[orchestrator][view][bulk]")
{
    auto const engine = bootEngine(workWorld());
    auto items = itemsIn(engine, WorkGroup::NeedsYou);
    std::erase_if(items, [](WorkItem const& item) { return item.agentRun.empty(); });
    CHECK_FALSE(openBulkDialog(engine, WorkGroup::NeedsYou, items).has_value());
}

TEST_CASE("Bulk dialog: the palette entry opens it on the selected line's group",
          "[orchestrator][view][bulk]")
{
    auto fixture = DashboardFixture { workWorld() };
    CHECK(fixture.dashboard.input("w").empty());
    CHECK(fixture.dashboard.input("\x0b").empty());
    CHECK(fixture.dashboard.input("run a task on").empty());
    CHECK(fixture.dashboard.input("\r").empty());
    auto const& interaction = fixture.dashboard.interaction();
    REQUIRE(interaction.bulk.has_value());
    CHECK(interaction.bulk->group == WorkGroup::NeedsYou);
    CHECK(fixture.screen().contains("Run a task on Needs you"));
}
