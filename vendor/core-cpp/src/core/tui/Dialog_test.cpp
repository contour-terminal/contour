// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Buffer.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/Dialog.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/TestHelpers.hpp>
#include <core/tui/Theme.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace core::tui;
using core::tui::test::canvasToString;
using core::tui::test::graphemeAt;

namespace
{

/// A canvas over its own buffer, so a case can render a dialog and read the cells back.
class Screenful
{
  public:
    Screenful(int cols, int rows):
        _buffer(rows, cols), _canvas(_buffer, Rect { .x = 0, .y = 0, .width = cols, .height = rows }, _theme)
    {
        _buffer.clear();
    }

    [[nodiscard]] auto canvas() noexcept -> Canvas& { return _canvas; }
    [[nodiscard]] auto buffer() const noexcept -> Buffer const& { return _buffer; }

  private:
    Buffer _buffer;
    Theme _theme;
    Canvas _canvas;
};

auto selectConfig() -> SelectDialogConfig
{
    auto config = SelectDialogConfig {};
    config.items = { ListItem { .label = "alpha" }, ListItem { .label = "beta" } };
    config.width = 20;
    config.border = BorderStyle::Single;
    config.dimBackground = false;
    return config;
}

auto confirmConfig() -> ConfirmDialogConfig
{
    auto config = ConfirmDialogConfig {};
    config.message = "Delete?";
    config.width = 20;
    config.border = BorderStyle::Single;
    config.dimBackground = false;
    return config;
}

auto inputConfig() -> InputDialogConfig
{
    auto config = InputDialogConfig {};
    config.prompt = "Name:";
    config.width = 20;
    config.border = BorderStyle::Single;
    config.dimBackground = false;
    return config;
}

} // namespace

// =============================================================================
// Frame placement
//
// Rect::x is the left column and Rect::y the top row, while putString() and
// setCursor() take (row, col). Every dialog built its frame from the two in the
// wrong order, so the border landed at (column = top row, row = left column)
// while its contents were placed correctly. The two coincide whenever the
// dialog happens to be centred at the same offset in both axes, which is why
// each case below picks a canvas and a width that keep them apart.
// =============================================================================

TEST_CASE("Dialog.ConfirmDialog_frame_is_at_column_and_row", "[tui][dialog]")
{
    auto screen = Screenful { 40, 20 };
    auto dialog = ConfirmDialog { confirmConfig() };
    dialog.render(screen.canvas());

    // dialogWidth 20, dialogHeight 6 => startCol (40-20)/2 = 10, startRow (20-6)/2 = 7.
    CHECK(graphemeAt(screen.buffer(), 7, 10) == "┌");  // top-left
    CHECK(graphemeAt(screen.buffer(), 7, 29) == "┐");  // top-right
    CHECK(graphemeAt(screen.buffer(), 12, 10) == "└"); // bottom-left
    CHECK(graphemeAt(screen.buffer(), 12, 29) == "┘"); // bottom-right

    // The message goes through putString(row, col) and was always placed right; the point of the
    // case is that the frame now encloses it instead of sitting below and to the left.
    CHECK(graphemeAt(screen.buffer(), 9, 13) == "D");

    // And nothing is drawn at the transposed corner.
    CHECK(graphemeAt(screen.buffer(), 10, 7) != "┌");
}

TEST_CASE("Dialog.SelectDialog_frame_is_at_column_and_row", "[tui][dialog]")
{
    auto screen = Screenful { 40, 20 };
    auto dialog = SelectDialog { selectConfig() };
    dialog.render(screen.canvas());

    // contentHeight 2, dialogHeight 4, dialogWidth 20 => startCol 10, startRow 8.
    CHECK(graphemeAt(screen.buffer(), 8, 10) == "┌");  // top-left
    CHECK(graphemeAt(screen.buffer(), 8, 29) == "┐");  // top-right
    CHECK(graphemeAt(screen.buffer(), 11, 10) == "└"); // bottom-left
    CHECK(graphemeAt(screen.buffer(), 11, 29) == "┘"); // bottom-right

    // The list's subcanvas starts one row down and two columns in, inside the frame.
    CHECK(canvasToString(screen.buffer(), Rect { .x = 12, .y = 9, .width = 16, .height = 1 })
              .contains("alpha"));
}

TEST_CASE("Dialog.InputDialog_frame_is_at_column_and_row", "[tui][dialog]")
{
    auto screen = Screenful { 40, 20 };
    auto dialog = InputDialog { inputConfig() };
    dialog.render(screen.canvas());

    // dialogWidth 20, dialogHeight 5 => startCol 10, startRow 7.
    CHECK(graphemeAt(screen.buffer(), 7, 10) == "┌");  // top-left
    CHECK(graphemeAt(screen.buffer(), 11, 29) == "┘"); // bottom-right

    // The prompt sits on the first inner row, two columns in.
    CHECK(graphemeAt(screen.buffer(), 8, 12) == "N");
}

// =============================================================================
// Narrow terminals
// =============================================================================

TEST_CASE("Dialog.InputDialog_renders_on_a_terminal_narrower_than_its_border", "[tui][dialog]")
{
    // Three columns leaves `termCols - 4` negative. That width reached the input field's
    // arithmetic unclamped, and `displayValue.size() - static_cast<std::size_t>(inputWidth)`
    // wrapped past the end of the string, so substr() threw std::out_of_range out of a render()
    // no caller expects to throw. The canvas clips everything else, so the box and the strings
    // simply do not appear.
    auto screen = Screenful { 3, 10 };
    auto dialog = InputDialog { inputConfig() };
    dialog.setValue("hello");

    CHECK_NOTHROW(dialog.render(screen.canvas()));
}

TEST_CASE("Dialog.every_dialog_renders_on_a_terminal_narrower_than_its_border", "[tui][dialog]")
{
    auto screen = Screenful { 3, 10 };

    auto select = SelectDialog { selectConfig() };
    CHECK_NOTHROW(select.render(screen.canvas()));

    auto confirm = ConfirmDialog { confirmConfig() };
    CHECK_NOTHROW(confirm.render(screen.canvas()));
}
