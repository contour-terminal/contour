// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Box.hpp>
#include <core/tui/Buffer.hpp>
#include <core/tui/ByteSinkOutput.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/CommandPalettePopup.hpp>
#include <core/tui/CommandRegistry.hpp>
#include <core/tui/CompletionPopup.hpp>
#include <core/tui/Dialog.hpp>
#include <core/tui/FuzzyPickerPopup.hpp>
#include <core/tui/QuestionComponent.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/TestHelpers.hpp>
#include <core/tui/Theme.hpp>
#include <core/tui/Tooltip.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace core::tui;
using core::tui::test::canvasToString;

namespace
{

/// Renders a component on a blank canvas of the given size and returns the cells, row by row.
auto renderComponent(Component& component, Size size) -> std::string
{
    auto buffer = Buffer(size.height, size.width);
    buffer.clear();
    auto const theme = Theme {};
    auto canvas = Canvas(buffer, Rect { .x = 0, .y = 0, .width = size.width, .height = size.height }, theme);
    component.render(canvas);
    return canvasToString(buffer);
}

/// The length of the UTF-8 sequence a lead byte starts.
auto sequenceLength(unsigned char lead) -> std::size_t
{
    if (lead < 0xC0U)
        return 1;
    if (lead < 0xE0U)
        return 2;
    if (lead < 0xF0U)
        return 3;
    return 4;
}

/// Replays the bytes a TerminalOutput wrote onto a grid: CSI row;col H moves, every other CSI is
/// skipped, every other byte is text. Enough for what Box writes.
auto replay(std::string_view bytes, Size size) -> std::string
{
    auto grid = std::vector<std::vector<std::string>>(
        static_cast<std::size_t>(size.height),
        std::vector<std::string>(static_cast<std::size_t>(size.width), " "));
    auto row = 0;
    auto col = 0;
    auto position = std::size_t { 0 };
    while (position < bytes.size())
    {
        if (bytes.substr(position).starts_with("\033["))
        {
            auto const end =
                bytes.find_first_of("@ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz~", position + 2);
            auto const parameters = bytes.substr(position + 2, end - position - 2);
            if (bytes[end] == 'H')
            {
                auto const separator = parameters.find(';');
                row = std::stoi(std::string(parameters.substr(0, separator))) - 1;
                col = std::stoi(std::string(parameters.substr(separator + 1))) - 1;
            }
            position = end + 1;
            continue;
        }
        auto const length = sequenceLength(static_cast<unsigned char>(bytes[position]));
        if (row >= 0 && row < size.height && col >= 0 && col < size.width)
            grid[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] =
                std::string(bytes.substr(position, length));
        ++col;
        position += length;
    }
    auto text = std::string {};
    for (auto const& line: grid)
    {
        if (!text.empty())
            text += '\n';
        for (auto const& cell: line)
            text += cell;
    }
    return text;
}

/// One widget that draws a border, rendered in a given style.
struct BorderedWidget
{
    std::string_view name;
    std::function<std::string(BorderStyle)> render;
};

auto borderedWidgets() -> std::vector<BorderedWidget>
{
    return {
        { "Box",
          [](BorderStyle border) {
              auto bytes = std::string {};
              auto output = ByteSinkOutput { [&bytes](std::string_view chunk) { bytes += chunk; },
                                             { .width = 14, .height = 4 },
                                             SinkDestination::Plain };
              auto config = BoxConfig {};
              config.width = 14;
              config.height = 4;
              config.border = border;
              config.title = "Box";
              Box(config).render(output);
              output.flush();
              return replay(bytes, { .width = 14, .height = 4 });
          } },
        { "SelectDialog",
          [](BorderStyle border) {
              auto config = SelectDialogConfig {};
              config.title = "Pick";
              config.items = { ListItem { .label = "alpha" }, ListItem { .label = "beta" } };
              config.width = 16;
              config.border = border;
              config.dimBackground = false;
              auto dialog = SelectDialog { std::move(config) };
              return renderComponent(dialog, { .width = 16, .height = 6 });
          } },
        { "ConfirmDialog",
          [](BorderStyle border) {
              auto config = ConfirmDialogConfig {};
              config.title = "Sure";
              config.message = "Delete?";
              config.width = 22;
              config.border = border;
              config.dimBackground = false;
              auto dialog = ConfirmDialog { std::move(config) };
              return renderComponent(dialog, { .width = 22, .height = 7 });
          } },
        { "InputDialog",
          [](BorderStyle border) {
              auto config = InputDialogConfig {};
              config.title = "Ask";
              config.prompt = "Name:";
              config.width = 16;
              config.border = border;
              config.dimBackground = false;
              auto dialog = InputDialog { std::move(config) };
              return renderComponent(dialog, { .width = 16, .height = 7 });
          } },
        { "CompletionPopup",
          [](BorderStyle border) {
              // With a detail panel, whose frame joins the menu's through T-junctions.
              auto popup = CompletionPopup { border };
              popup.show(test::makeItemsWithDetail({ { "alpha", "", "first" }, { "beta", "", "" } }));
              return renderComponent(popup, { .width = 40, .height = 4 });
          } },
        { "CommandPalettePopup",
          [](BorderStyle border) {
              auto popup = CommandPalettePopup { border };
              popup.showSubMenu({ CommandEntry { .id = "a",
                                                 .label = "Open",
                                                 .description = {},
                                                 .category = {},
                                                 .keybinding = {},
                                                 .context = {},
                                                 .action = {} } },
                                "Pick");
              return renderComponent(popup, { .width = 24, .height = 5 });
          } },
        { "FuzzyPickerPopup",
          [](BorderStyle border) {
              auto popup = FuzzyPickerPopup { border };
              popup.show({ "alpha", "beta" }, "Pick");
              return renderComponent(popup, { .width = 20, .height = 6 });
          } },
        { "Tooltip",
          [](BorderStyle border) {
              auto tooltip = Tooltip { border };
              tooltip.setContent("hint");
              return renderComponent(tooltip, tooltip.preferredSize());
          } },
        { "QuestionComponent",
          [](BorderStyle border) {
              auto config = QuestionConfig {};
              config.questionText = "Go?";
              config.options = { "yes", "no" };
              config.allowOther = false;
              config.border = border;
              auto question = QuestionComponent { std::move(config) };
              return renderComponent(question, { .width = 16, .height = 6 });
          } },
    };
}

/// Each row of a rendering without its trailing blanks, and no blank rows after the last drawn one.
auto trimmed(std::string const& text) -> std::vector<std::string>
{
    auto rows = std::vector<std::string> {};
    for (auto const row: std::views::split(text, '\n'))
    {
        auto line = std::string(std::string_view(row.begin(), row.end()));
        while (line.ends_with(' '))
            line.pop_back();
        rows.push_back(std::move(line));
    }
    while (!rows.empty() && rows.back().empty())
        rows.pop_back();
    return rows;
}

/// A widget's golden rendering with BorderStyle::Rounded.
struct Golden
{
    std::string_view name;
    std::vector<std::string> rows;
};

/// The four corners of each style that has them. Single and Dashed share theirs.
struct Corners
{
    BorderStyle style = BorderStyle::None;
    std::array<std::string_view, 4> corners;
};

constexpr auto CornerTable = std::array {
    Corners { .style = BorderStyle::Single, .corners = { "┌", "┐", "└", "┘" } },
    Corners { .style = BorderStyle::Rounded, .corners = { "╭", "╮", "╰", "╯" } },
    Corners { .style = BorderStyle::Double, .corners = { "╔", "╗", "╚", "╝" } },
    Corners { .style = BorderStyle::Heavy, .corners = { "┏", "┓", "┗", "┛" } },
};

} // namespace

TEST_CASE("Borders: every bordered widget draws BorderStyle::Rounded with rounded corners (goldens)")
{
    // clang-format off
    auto const goldens = std::array {
        Golden { "Box",
                 {
                     "╭─Box────────╮",
                     "│            │",
                     "│            │",
                     "╰────────────╯",
                 } },
        Golden { "SelectDialog",
                 {
                     "",
                     "  ╭───Pick───╮",
                     "  │ ▶ alpha  │",
                     "  │   beta   │",
                     "  ╰──────────╯",
                 } },
        Golden { "ConfirmDialog",
                 {
                     "  ╭──────Sure──────╮",
                     "  │                │",
                     "  │  Delete?       │",
                     "  │                │",
                     "  │ [ Yes ]  [ No ]│",
                     "  ╰────────────────╯",
                 } },
        Golden { "InputDialog",
                 {
                     "",
                     "  ╭───Ask────╮",
                     "  │ Name:    │",
                     "  │          │",
                     "  │          │",
                     "  ╰──────────╯",
                 } },
        Golden { "CompletionPopup",
                 {
                     "╭───────┬───────────────────╮",
                     "│alpha  │first              │",
                     "│beta   │                   │",
                     "╰───────┴───────────────────╯",
                 } },
        Golden { "CommandPalettePopup",
                 {
                     "╭──────────────────────╮",
                     "│Pick>                 │",
                     "│──────────────────────│",
                     "│Open                  │",
                     "╰──────────────────────╯",
                 } },
        Golden { "FuzzyPickerPopup",
                 {
                     "╭──────────────────╮",
                     "│Pick              │",
                     "│──────────────────│",
                     "│alpha             │",
                     "│beta              │",
                     "╰──────────────────╯",
                 } },
        Golden { "Tooltip",
                 {
                     "╭──────╮",
                     "│ hint │",
                     "╰──────╯",
                 } },
        Golden { "QuestionComponent",
                 {
                     "╭─ question ────",
                     "│  Go?",
                     "│",
                     "│  ▶ yes",
                     "│    no",
                     "╰─ Enter select",
                 } },
    };
    // clang-format on

    auto const widgets = borderedWidgets();
    REQUIRE(widgets.size() == goldens.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, widgets.size()))
    {
        auto const& widget = widgets[index];
        auto const& golden = goldens[index];
        INFO(widget.name);
        REQUIRE(widget.name == golden.name);
        CHECK(trimmed(widget.render(BorderStyle::Rounded)) == golden.rows);
    }
}

// The test that no widget hard-codes a corner is a rendering check, not a source scan: each widget is
// rendered in every style that has corners, and no corner of another style may appear. A scan for the
// corner code points would miss `BorderChars::fromStyle(BorderStyle::Single)` written into a widget,
// which is how CompletionPopup's detail panel drew its corners square whatever its frame; a rendering
// cannot. A new bordered widget joins borderedWidgets() above.
TEST_CASE("Borders: no widget hard-codes a corner of another style")
{
    for (auto const& widget: borderedWidgets())
    {
        for (auto const& own: CornerTable)
        {
            auto const rendered = widget.render(own.style);
            INFO(widget.name << " drawn in style " << static_cast<int>(own.style) << ":\n" << rendered);
            auto drewOwnCorner = false;
            for (auto const corner: own.corners)
                drewOwnCorner = drewOwnCorner || rendered.contains(corner);
            CHECK(drewOwnCorner);
            for (auto const& other: CornerTable)
            {
                if (other.style == own.style)
                    continue;
                for (auto const corner: other.corners)
                    CHECK_FALSE(rendered.contains(corner));
            }
        }
    }
}
