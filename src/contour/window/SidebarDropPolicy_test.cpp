// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarDropPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace contour::window;

namespace
{
SidebarRow rowOf(SidebarRowKind kind, uint64_t workspace, uint64_t tab, std::size_t position)
{
    auto row = SidebarRow {};
    row.key = { .kind = kind, .workspace = workspace, .tab = tab, .pane = {} };
    row.position = position;
    return row;
}

/// Workspaces 10 (tabs 1 split, 2) and 20 (tab 1) as shown, the rows at indices 0..5.
std::vector<SidebarRow> sampleRows()
{
    return { rowOf(SidebarRowKind::Workspace, 10, 0, 0), rowOf(SidebarRowKind::Tab, 10, 1, 0),
             rowOf(SidebarRowKind::Pane, 10, 1, 0),      rowOf(SidebarRowKind::Tab, 10, 2, 1),
             rowOf(SidebarRowKind::Workspace, 20, 0, 1), rowOf(SidebarRowKind::Tab, 20, 1, 0) };
}

/// The outcome in one string: `reorder N`, `move W@P` (`W@end` for the end), or `refuse`.
std::string describe(SidebarDrop const& drop)
{
    if (auto const* reorder = std::get_if<ReorderWorkspace>(&drop))
        return "reorder " + std::to_string(reorder->to);
    if (auto const* move = std::get_if<MoveTab>(&drop))
        return "move " + std::to_string(move->toWorkspace) + "@"
               + (move->toPosition == AppendTab ? std::string { "end" } : std::to_string(move->toPosition));
    return "refuse";
}
} // namespace

TEST_CASE("SidebarDropPolicy.subject_by_spot", "[sidebar]")
{
    struct Case
    {
        int row;
        double y;
        DragSubject subject;
        std::string outcome;
    };
    using enum DragSubject;
    auto const cases = std::vector<Case> {
        // Workspace row (index 4, workspace 20 at position 1; index 0 is the first).
        { 4, 0.1, Workspace, "reorder 1" },
        { 4, 0.4, Workspace, "reorder 1" },
        { 4, 0.6, Workspace, "reorder 2" },
        { 4, 0.9, Workspace, "reorder 2" },
        { 4, 0.1, Tab, "move 10@end" },
        { 4, 0.5, Tab, "move 20@end" },
        { 4, 0.9, Tab, "move 20@0" },
        { 0, 0.1, Tab, "move 10@0" }, // the first row has no workspace above it
        { 0, 0.5, Tab, "move 10@end" },
        { 4, 0.1, Pane, "refuse" },
        { 4, 0.5, Pane, "refuse" },
        { 4, 0.9, Pane, "refuse" },
        // Tab row (index 3, tab 2 at position 1 of workspace 10).
        { 3, 0.1, Workspace, "refuse" },
        { 3, 0.9, Workspace, "refuse" },
        { 3, 0.2, Tab, "move 10@1" },
        { 3, 0.49, Tab, "move 10@1" },
        { 3, 0.51, Tab, "move 10@2" },
        { 3, 0.9, Tab, "move 10@2" },
        { 3, 0.2, Pane, "refuse" },
        { 3, 0.9, Pane, "refuse" },
        // Pane row (index 2).
        { 2, 0.1, Workspace, "refuse" },
        { 2, 0.5, Tab, "refuse" },
        { 2, 0.9, Pane, "refuse" },
        // Out of range.
        { -1, 0.5, Tab, "refuse" },
        { 6, 0.5, Tab, "refuse" },
    };
    auto const rows = sampleRows();
    for (auto const& [row, y, subject, outcome]: cases)
    {
        INFO("row " << row << " y " << y << " subject " << static_cast<int>(subject));
        CHECK(describe(resolveSidebarDrop(rows, row, y, subject)) == outcome);
    }
    CHECK(describe(resolveSidebarDrop({}, 0, 0.5, Tab)) == "refuse");
}

TEST_CASE("SidebarDropPolicy.positions_are_real_not_visible_indices", "[sidebar]")
{
    // A filter shows only workspace 20 (position 3) and its tab (position 7): the drop lands there.
    auto const rows =
        std::vector { rowOf(SidebarRowKind::Workspace, 20, 0, 3), rowOf(SidebarRowKind::Tab, 20, 9, 7) };
    CHECK(describe(resolveSidebarDrop(rows, 0, 0.1, DragSubject::Workspace)) == "reorder 3");
    CHECK(describe(resolveSidebarDrop(rows, 0, 0.9, DragSubject::Workspace)) == "reorder 4");
    CHECK(describe(resolveSidebarDrop(rows, 1, 0.1, DragSubject::Tab)) == "move 20@7");
    CHECK(describe(resolveSidebarDrop(rows, 1, 0.9, DragSubject::Tab)) == "move 20@8");
}

TEST_CASE("SidebarDropPolicy.a_dropped_item_ends_where_it_was_dropped_before", "[sidebar]")
{
    // Down past itself it lands one place before the gap it was dropped in; up, in the gap.
    CHECK(placeAfterMove(0, 0) == 0);
    CHECK(placeAfterMove(0, 1) == 0);
    CHECK(placeAfterMove(0, 3) == 2);
    CHECK(placeAfterMove(2, 0) == 0);

    // A tab from another workspace lands in the gap, or at the end.
    CHECK(tabPlaceAfterMove(std::nullopt, 1, 3) == 1);
    CHECK(tabPlaceAfterMove(std::nullopt, AppendTab, 3) == 3);
    // Within its own workspace it leaves its place first.
    CHECK(tabPlaceAfterMove(0, 2, 3) == 1);
    CHECK(tabPlaceAfterMove(2, 0, 3) == 0);
    CHECK(tabPlaceAfterMove(1, AppendTab, 3) == 2);
    CHECK(tabPlaceAfterMove(1, 9, 3) == 2);
}
