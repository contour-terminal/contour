// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/TreeTableView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <ranges>
#include <string>
#include <type_traits>
#include <vector>

using core::ManualClock;
using namespace core::tui;

namespace
{
/// A tiny in-memory tree model for exercising the generic view without any domain code.
///
/// Nodes are ints; children are stored in a map. The "current node" plus a parent stack
/// model navigation. Sorting just reverses the child order so re-sorts are observable.
class FakeModel: public TreeTableModel
{
  public:
    FakeModel()
    {
        _children[0] = { 1, 2, 3 }; // root's children
        _children[2] = { 4, 5 };    // node 2 is a directory
        _names = { { 0, "root" }, { 1, "alpha" }, { 2, "dir" }, { 3, "beta" }, { 4, "x" }, { 5, "y" } };
    }

    [[nodiscard]] std::vector<TableColumn> columns() const override
    {
        return { TableColumn {
                     .header = "Name", .width = ColumnWidth::Flex, .size = 4, .align = ColumnAlign::Left },
                 TableColumn {
                     .header = "Id", .width = ColumnWidth::Fixed, .size = 3, .align = ColumnAlign::Right } };
    }

    [[nodiscard]] std::vector<RowId> rows() const override
    {
        std::vector<RowId> out;
        for (auto const id: _children.at(_current))
            out.push_back(static_cast<RowId>(id));
        return out;
    }

    [[nodiscard]] std::string cellText(RowId row, std::size_t column) const override
    {
        if (column == 0)
            return _names.at(static_cast<int>(row));
        return std::to_string(row);
    }

    [[nodiscard]] RowStyle rowStyle(RowId, bool selected) const override
    {
        return RowStyle { .style = {}, .selected = selected };
    }

    [[nodiscard]] bool canDescend(RowId row) const override
    {
        return _children.contains(static_cast<int>(row));
    }

    bool descend(RowId row) override
    {
        if (!canDescend(row))
            return false;
        _stack.push_back(_current);
        _current = static_cast<int>(row);
        return true;
    }

    bool ascend() override
    {
        if (_stack.empty())
            return false;
        _current = _stack.back();
        _stack.pop_back();
        return true;
    }

    [[nodiscard]] std::string currentTitle() const override { return _names.at(_current); }

    void sortBy(int) override
    {
        auto& kids = _children[_current];
        std::ranges::reverse(kids);
    }

    [[nodiscard]] int current() const { return _current; }

  private:
    std::map<int, std::vector<int>> _children;
    std::map<int, std::string> _names;
    std::vector<int> _stack;
    int _current = 0;
};
} // namespace

TEST_CASE("TreeTableView: cursor moves within row bounds", "[treetable]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    CHECK(view.cursorIndex() == 0);
    view.moveCursor(1);
    CHECK(view.cursorIndex() == 1);
    view.moveCursor(100); // clamps to last (3 rows -> index 2)
    CHECK(view.cursorIndex() == 2);
    view.moveCursor(-100); // clamps to first
    CHECK(view.cursorIndex() == 0);
}

TEST_CASE("TreeTableView: moveToTop/Bottom", "[treetable]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    view.moveToBottom();
    CHECK(view.cursorIndex() == 2);
    view.moveToTop();
    CHECK(view.cursorIndex() == 0);
}

TEST_CASE("TreeTableView: descend into a directory row resets cursor", "[treetable]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    // Move to node 2 ("dir", index 1) and descend.
    view.moveCursor(1);
    bool valid = false;
    CHECK(view.cursorRow(valid) == 2);
    view.descendCursor();
    CHECK(model.current() == 2);
    CHECK(view.cursorIndex() == 0); // reset on descend
}

TEST_CASE("TreeTableView: descend is a no-op on a leaf row", "[treetable]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    // Row 0 is node 1 ("alpha"), a leaf.
    view.descendCursor();
    CHECK(model.current() == 0); // unchanged
}

TEST_CASE("TreeTableView: ascend restores the remembered cursor", "[treetable]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    view.moveCursor(1); // cursor on "dir" (index 1)
    view.descendCursor();
    REQUIRE(model.current() == 2);
    view.ascendCursor();
    CHECK(model.current() == 0);
    CHECK(view.cursorIndex() == 1); // restored to where we descended from
}

TEST_CASE("TreeTableView: sortBy keeps the cursor on the same row", "[treetable]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    // Cursor on row index 0 = node 1. After reverse-sort, node 1 is last.
    bool valid = false;
    REQUIRE(view.cursorRow(valid) == 1);
    view.sortBy(0);
    CHECK(view.cursorRow(valid) == 1); // same row id
    CHECK(view.cursorIndex() == 2);    // now at the end
}

TEST_CASE("TreeTableView: selection-changed callback fires on navigation", "[treetable]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    int changes = 0;
    view.onSelectionChanged([&] { ++changes; });
    view.moveCursor(1);
    view.descendCursor();
    CHECK(changes >= 2);
}

TEST_CASE("TreeTableView: handles j/k/l/h key events", "[treetable]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    auto keyChar = [](char32_t c) {
        return InputEvent { KeyEvent { .key = {}, .modifiers = Modifier::None, .codepoint = c } };
    };

    CHECK(view.onEvent(keyChar(U'j')) == EventResult::Handled);
    CHECK(view.cursorIndex() == 1);
    CHECK(view.onEvent(keyChar(U'k')) == EventResult::Handled);
    CHECK(view.cursorIndex() == 0);
    CHECK(view.onEvent(keyChar(U'z')) == EventResult::Ignored);
}

namespace
{
/// Builds a left-button press at component-relative, 1-based row @p y.
[[nodiscard]] InputEvent mousePress(int y)
{
    return InputEvent { MouseEvent { .type = MouseEvent::Type::Press, .button = 0, .x = 1, .y = y } };
}
} // namespace

TEST_CASE("TreeTableView: left-click selects the row under the pointer", "[treetable][mouse]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    // Header occupies row 1; the first data row is row 2. Clicking row 3 selects index 1.
    CHECK(view.onEvent(mousePress(3)) == EventResult::Handled);
    CHECK(view.cursorIndex() == 1);
    CHECK(view.onEvent(mousePress(2)) == EventResult::Handled);
    CHECK(view.cursorIndex() == 0);
}

TEST_CASE("TreeTableView: double-click activates (descends into) the row", "[treetable][mouse]")
{
    FakeModel model;
    auto const clock = ManualClock {}; // Frozen: both presses land inside the double-click window.
    TreeTableView view(model, {}, clock);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    // Row 3 is index 1 = node 2 ("dir"), which is descendable. Two quick presses descend.
    CHECK(view.onEvent(mousePress(3)) == EventResult::Handled);
    REQUIRE(model.current() == 0); // first click only selects
    CHECK(view.onEvent(mousePress(3)) == EventResult::Handled);
    CHECK(model.current() == 2);    // second click activates
    CHECK(view.cursorIndex() == 0); // descend resets the cursor
}

TEST_CASE("TreeTableView: two clicks on different rows do not activate", "[treetable][mouse]")
{
    FakeModel model;
    auto const clock = ManualClock {}; // Frozen: both presses land inside the double-click window.
    TreeTableView view(model, {}, clock);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    CHECK(view.onEvent(mousePress(3)) == EventResult::Handled); // index 1 (dir)
    CHECK(view.onEvent(mousePress(4)) == EventResult::Handled); // index 2 (leaf) — different row
    CHECK(model.current() == 0);                                // no descend
    CHECK(view.cursorIndex() == 2);
}

TEST_CASE("TreeTableView: a double-click is timed on the injected clock", "[treetable][mouse]")
{
    FakeModel model;
    auto clock = ManualClock {};
    TreeTableView view(model, {}, clock);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    SECTION("a second press 400 ms later activates")
    {
        CHECK(view.onEvent(mousePress(3)) == EventResult::Handled);
        clock.advance(std::chrono::milliseconds(400));
        CHECK(view.onEvent(mousePress(3)) == EventResult::Handled);
        CHECK(model.current() == 2);
    }

    SECTION("a second press 401 ms later only selects, and starts a new double-click")
    {
        CHECK(view.onEvent(mousePress(3)) == EventResult::Handled);
        clock.advance(std::chrono::milliseconds(401));
        CHECK(view.onEvent(mousePress(3)) == EventResult::Handled);
        CHECK(model.current() == 0);
        clock.advance(std::chrono::milliseconds(100));
        CHECK(view.onEvent(mousePress(3)) == EventResult::Handled);
        CHECK(model.current() == 2);
    }
}

TEST_CASE("TreeTableView: clicks on the header or below the list are ignored", "[treetable][mouse]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    view.moveCursor(1);
    REQUIRE(view.cursorIndex() == 1);
    CHECK(view.onEvent(mousePress(1)) == EventResult::Ignored);  // header row
    CHECK(view.onEvent(mousePress(15)) == EventResult::Ignored); // below the 3 rows
    CHECK(view.cursorIndex() == 1);                              // selection unchanged
}

TEST_CASE("TreeTableView: wheel scroll moves the selection", "[treetable][mouse]")
{
    FakeModel model;
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 10 });

    auto wheel = [](MouseEvent::Type type) {
        return InputEvent { MouseEvent { .type = type, .button = 0, .x = 1, .y = 1 } };
    };

    CHECK(view.onEvent(wheel(MouseEvent::Type::ScrollDown)) == EventResult::Handled);
    CHECK(view.cursorIndex() == 1);
    CHECK(view.onEvent(wheel(MouseEvent::Type::ScrollUp)) == EventResult::Handled);
    CHECK(view.cursorIndex() == 0);
}

namespace
{
/// A flat model with @p n rows and no children, to exercise paging motions.
class FlatModel: public TreeTableModel
{
  public:
    explicit FlatModel(int n): _n(n) {}

    [[nodiscard]] std::vector<TableColumn> columns() const override
    {
        return { TableColumn {
            .header = "N", .width = ColumnWidth::Flex, .size = 4, .align = ColumnAlign::Left } };
    }

    [[nodiscard]] std::vector<RowId> rows() const override
    {
        std::vector<RowId> out;
        out.reserve(static_cast<std::size_t>(_n));
        for (auto const i: std::views::iota(0, std::max(0, _n)))
            out.push_back(static_cast<RowId>(i));
        return out;
    }

    [[nodiscard]] std::string cellText(RowId row, std::size_t) const override { return std::to_string(row); }

    [[nodiscard]] RowStyle rowStyle(RowId, bool sel) const override
    {
        return RowStyle { .style = {}, .selected = sel };
    }

    [[nodiscard]] bool canDescend(RowId) const override { return false; }

    bool descend(RowId) override { return false; }

    bool ascend() override { return false; }

    [[nodiscard]] std::string currentTitle() const override { return "/"; }

    void sortBy(int) override {}

  private:
    int _n;
};
} // namespace

TEST_CASE("TreeTableView: halfPageBy moves half the viewport", "[treetable]")
{
    FlatModel model(100);
    TreeTableView view(model);
    // Header takes 1 line; viewport = height - 1 = 20; half = 10.
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 21 });

    view.halfPageBy(1);
    CHECK(view.cursorIndex() == 10);
    view.halfPageBy(1);
    CHECK(view.cursorIndex() == 20);
    view.halfPageBy(-1);
    CHECK(view.cursorIndex() == 10);
}

TEST_CASE("TreeTableView: pageBy moves a full viewport", "[treetable]")
{
    FlatModel model(100);
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 21 }); // viewport 20

    view.pageBy(1);
    CHECK(view.cursorIndex() == 20);
    view.pageBy(-1);
    CHECK(view.cursorIndex() == 0);
}

TEST_CASE("TreeTableView: Ctrl-D / Ctrl-U handled in onEvent", "[treetable]")
{
    FlatModel model(100);
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 21 }); // half-page = 10

    auto ctrlKey = [](char32_t c) {
        return InputEvent { KeyEvent { .key = {}, .modifiers = Modifier::Ctrl, .codepoint = c } };
    };

    CHECK(view.onEvent(ctrlKey(U'd')) == EventResult::Handled);
    CHECK(view.cursorIndex() == 10);
    CHECK(view.onEvent(ctrlKey(U'u')) == EventResult::Handled);
    CHECK(view.cursorIndex() == 0);
}

TEST_CASE("TreeTableView: g / G jump to top / bottom in onEvent", "[treetable]")
{
    FlatModel model(50);
    TreeTableView view(model);
    view.setArea(Rect { .x = 0, .y = 0, .width = 20, .height = 21 });

    auto key = [](char32_t c, Modifier m = Modifier::None) {
        return InputEvent { KeyEvent { .key = {}, .modifiers = m, .codepoint = c } };
    };

    CHECK(view.onEvent(key(U'G')) == EventResult::Handled);
    CHECK(view.cursorIndex() == 49);
    CHECK(view.onEvent(key(U'g')) == EventResult::Handled);
    CHECK(view.cursorIndex() == 0);
}

TEST_CASE("TreeTableView: a temporary clock is refused", "[treetable]")
{
    STATIC_CHECK(std::is_constructible_v<TreeTableView, TreeTableModel&, TreeTableConfig, ManualClock&>);
    STATIC_CHECK_FALSE(std::is_constructible_v<TreeTableView, TreeTableModel&, TreeTableConfig, ManualClock>);
}
