// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarDropPolicy.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <ranges>
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

/// The outcome in one string: `reorder N`, `move W@P` (`W@end` for the end), `join W/T`, `new`, or `refuse`.
std::string describe(SidebarDrop const& drop)
{
    if (auto const* reorder = std::get_if<ReorderWorkspace>(&drop))
        return "reorder " + std::to_string(reorder->to);
    if (auto const* move = std::get_if<MoveTab>(&drop))
        return "move " + std::to_string(move->toWorkspace) + "@"
               + (move->toPosition == AppendTab ? std::string { "end" } : std::to_string(move->toPosition));
    if (auto const* join = std::get_if<JoinTab>(&drop))
        return "join " + std::to_string(join->tab.workspace) + "/" + std::to_string(join->tab.tab);
    if (std::holds_alternative<IntoNewWorkspace>(drop))
        return "new";
    return "refuse";
}

/// A workspace of several tabs.
constexpr auto AWorkspace = SidebarDragged { .subject = DragSubject::Workspace, .tab = {}, .share = {} };
/// Workspace 30, holding only tab 5.
constexpr auto OneTabbed = SidebarDragged { .subject = DragSubject::OneTabWorkspace,
                                            .tab = TabRef { 30, 5 },
                                            .share = WorkspaceShare::Whole };
/// Tab 7 of workspace 30, which has more.
constexpr auto ATab =
    SidebarDragged { .subject = DragSubject::Tab, .tab = TabRef { 30, 7 }, .share = WorkspaceShare::Part };
/// A pane of tab 7 of workspace 30.
constexpr auto APane =
    SidebarDragged { .subject = DragSubject::Pane, .tab = TabRef { 30, 7 }, .share = WorkspaceShare::Part };
} // namespace

TEST_CASE("SidebarDropPolicy.subject_by_spot", "[sidebar]")
{
    struct Case
    {
        int row;
        double y;
        SidebarDragged dragged;
        std::string outcome;
    };
    auto const cases = std::vector<Case> {
        // Workspace row (index 4, workspace 20 at position 1; index 0 is the first).
        { 4, 0.1, AWorkspace, "reorder 1" },
        { 4, 0.4, AWorkspace, "reorder 1" },
        { 4, 0.6, AWorkspace, "reorder 2" },
        { 4, 0.9, AWorkspace, "reorder 2" },
        { 4, 0.4, OneTabbed, "reorder 1" },
        { 4, 0.9, OneTabbed, "reorder 2" },
        { 4, 0.1, ATab, "move 10@end" },
        { 4, 0.5, ATab, "move 20@end" },
        { 4, 0.9, ATab, "move 20@0" },
        { 0, 0.1, ATab, "move 10@0" }, // the first row has no workspace above it
        { 0, 0.5, ATab, "move 10@end" },
        { 4, 0.1, APane, "move 10@end" },
        { 4, 0.5, APane, "move 20@end" },
        { 4, 0.9, APane, "move 20@0" },
        // Tab row (index 3, tab 2 at position 1 of workspace 10): gaps in the outer thirds, a join in the
        // middle.
        { 3, 0.1, AWorkspace, "refuse" },
        { 3, 0.5, AWorkspace, "refuse" },
        { 3, 0.9, AWorkspace, "refuse" },
        { 3, 0.1, OneTabbed, "refuse" },
        { 3, 0.5, OneTabbed, "join 10/2" },
        { 3, 0.9, OneTabbed, "refuse" },
        { 3, 0.2, ATab, "move 10@1" },
        { 3, 0.34, ATab, "join 10/2" },
        { 3, 0.5, ATab, "join 10/2" },
        { 3, 0.66, ATab, "join 10/2" },
        { 3, 0.9, ATab, "move 10@2" },
        { 3, 0.2, APane, "move 10@1" },
        { 3, 0.5, APane, "join 10/2" },
        { 3, 0.9, APane, "move 10@2" },
        // Pane row (index 2): nothing lands on a pane.
        { 2, 0.1, AWorkspace, "refuse" },
        { 2, 0.5, OneTabbed, "refuse" },
        { 2, 0.5, ATab, "refuse" },
        { 2, 0.9, APane, "refuse" },
        // Out of range.
        { -1, 0.5, ATab, "refuse" },
        { 6, 0.5, ATab, "refuse" },
    };
    auto const rows = sampleRows();
    for (auto const& [row, y, dragged, outcome]: cases)
    {
        INFO("row " << row << " y " << y << " subject " << static_cast<int>(dragged.subject));
        CHECK(describe(resolveSidebarDrop(rows, row, y, dragged)) == outcome);
    }
    CHECK(describe(resolveSidebarDrop({}, 0, 0.5, ATab)) == "refuse");
}

TEST_CASE("SidebarDropPolicy.a_join_into_its_own_tab_is_refused", "[sidebar]")
{
    // Tab 2 of workspace 10 is the row at index 3.
    auto const rows = sampleRows();
    auto const own = TabRef { 10, 2 };
    for (auto const subject: { DragSubject::Pane, DragSubject::Tab, DragSubject::OneTabWorkspace })
    {
        INFO(static_cast<int>(subject));
        auto const dragged = SidebarDragged { .subject = subject, .tab = own, .share = WorkspaceShare::Part };
        CHECK(describe(resolveSidebarDrop(rows, 3, 0.5, dragged)) == "refuse");
        CHECK(sidebarDropIndicator(rows, 3, 0.5, dragged) == DropIndicator::Refused);
    }
    // The gaps beside its own row still reorder it.
    CHECK(describe(resolveSidebarDrop(
              rows, 3, 0.1, SidebarDragged { .subject = DragSubject::Tab, .tab = own, .share = {} }))
          == "move 10@1");
}

TEST_CASE("SidebarDropPolicy.the_new_workspace_area_takes_what_is_not_all_of_one", "[sidebar]")
{
    struct Case
    {
        SidebarDragged dragged;
        std::string outcome;
    };
    auto const cases = std::vector<Case> {
        { APane, "new" },
        { ATab, "new" },
        { SidebarDragged {
              .subject = DragSubject::Tab, .tab = TabRef { 30, 5 }, .share = WorkspaceShare::Whole },
          "refuse" },
        { AWorkspace, "refuse" },
        { OneTabbed, "refuse" },
    };
    for (auto const& [dragged, outcome]: cases)
    {
        INFO(static_cast<int>(dragged.subject));
        CHECK(describe(resolveNewWorkspaceDrop(dragged)) == outcome);
    }
}

TEST_CASE("SidebarDropPolicy.the_indicator_shows_where_it_lands", "[sidebar]")
{
    struct Case
    {
        int row;
        double y;
        SidebarDragged dragged;
        DropIndicator indicator;
    };
    using enum DropIndicator;
    auto const cases = std::vector<Case> {
        { 4, 0.1, AWorkspace, LineAbove },  { 4, 0.45, AWorkspace, LineAbove },
        { 4, 0.55, AWorkspace, LineBelow }, { 4, 0.9, AWorkspace, LineBelow },
        { 4, 0.1, ATab, LineAbove },        { 4, 0.5, ATab, Highlight },
        { 4, 0.9, ATab, LineBelow },        { 3, 0.1, APane, LineAbove },
        { 3, 0.5, APane, Highlight },       { 3, 0.9, APane, LineBelow },
        { 3, 0.5, AWorkspace, Refused },    { 2, 0.5, APane, Refused },
        { 7, 0.5, APane, Refused },
    };
    auto const rows = sampleRows();
    for (auto const& [row, y, dragged, indicator]: cases)
    {
        INFO("row " << row << " y " << y << " subject " << static_cast<int>(dragged.subject));
        CHECK(sidebarDropIndicator(rows, row, y, dragged) == indicator);
    }
}

TEST_CASE("SidebarDropPolicy.every_rule_is_unique", "[sidebar]")
{
    for (auto const& [index, rule]: core::views::enumerate(SidebarDropRules))
        for (auto const& other: SidebarDropRules | std::views::drop(index + 1))
            CHECK_FALSE(
                (rule.subject == other.subject && rule.spot == other.spot && rule.band == other.band));
}

TEST_CASE("SidebarDropPolicy.positions_are_real_not_visible_indices", "[sidebar]")
{
    // A filter shows only workspace 20 (position 3) and its tab (position 7): the drop lands there.
    auto const rows =
        std::vector { rowOf(SidebarRowKind::Workspace, 20, 0, 3), rowOf(SidebarRowKind::Tab, 20, 9, 7) };
    CHECK(describe(resolveSidebarDrop(rows, 0, 0.1, AWorkspace)) == "reorder 3");
    CHECK(describe(resolveSidebarDrop(rows, 0, 0.9, AWorkspace)) == "reorder 4");
    CHECK(describe(resolveSidebarDrop(rows, 1, 0.1, ATab)) == "move 20@7");
    CHECK(describe(resolveSidebarDrop(rows, 1, 0.9, ATab)) == "move 20@8");
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

TEST_CASE("SidebarDropPolicy.a_host_short_of_node_moves_refuses_what_it_cannot_move", "[sidebar]")
{
    struct Case
    {
        int row;
        double y;
        DragSubject subject;
        std::string older; ///< Under an older daemon (HostMoves::Tabs).
        std::string tmux;  ///< Under tmux (HostMoves::Nothing).
    };
    auto const cases = std::vector<Case> {
        // Every host keeps its own order of workspaces.
        { 4, 0.1, DragSubject::Workspace, "reorder 1", "reorder 1" },
        { 4, 0.9, DragSubject::OneTabWorkspace, "reorder 2", "reorder 2" },
        // An older daemon moves no node from the sidebar: its MoveNode-less moves would all end "Not moved".
        { 4, 0.5, DragSubject::Tab, "refuse", "refuse" },
        { 3, 0.1, DragSubject::Tab, "refuse", "refuse" },
        { 3, 0.5, DragSubject::Tab, "refuse", "refuse" },
        { 3, 0.5, DragSubject::OneTabWorkspace, "refuse", "refuse" },
        { 4, 0.5, DragSubject::Pane, "refuse", "refuse" },
        { 3, 0.5, DragSubject::Pane, "refuse", "refuse" },
    };
    auto const rows = sampleRows();
    auto const draggedOn = [](DragSubject subject, HostMoves host) {
        return SidebarDragged {
            .subject = subject, .tab = TabRef { 30, 7 }, .share = WorkspaceShare::Part, .host = host
        };
    };
    for (auto const& [row, y, subject, older, tmux]: cases)
    {
        INFO("row " << row << " y " << y << " subject " << static_cast<int>(subject));
        CHECK(describe(resolveSidebarDrop(rows, row, y, draggedOn(subject, HostMoves::Tabs))) == older);
        CHECK(describe(resolveSidebarDrop(rows, row, y, draggedOn(subject, HostMoves::Nothing))) == tmux);
        CHECK((sidebarDropIndicator(rows, row, y, draggedOn(subject, HostMoves::Nothing))
               == DropIndicator::Refused)
              == (tmux == "refuse"));
    }
    // Neither takes a pane or a tab into a workspace of its own.
    for (auto const host: { HostMoves::Tabs, HostMoves::Nothing })
    {
        CHECK(describe(resolveNewWorkspaceDrop(draggedOn(DragSubject::Tab, host))) == "refuse");
        CHECK(describe(resolveNewWorkspaceDrop(draggedOn(DragSubject::Pane, host))) == "refuse");
    }
    CHECK(sidebarRefusalOf(HostMoves::Nothing) == MoveRefusal::Tmux);
    CHECK(sidebarRefusalOf(HostMoves::Tabs) == MoveRefusal::OldDaemon);
    CHECK(sidebarRefusalOf(HostMoves::Nodes) == MoveRefusal::None);
}
