// SPDX-License-Identifier: Apache-2.0
//
// The native2026 edition's indicator status line, drawn: the template the edition table ships is parsed
// into a terminal and checked against what the redesign's canvas shows, state by state.
#include <contour/config/Config.hpp>

#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/screen/StatusLineBuilder.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace vtbackend;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

namespace
{

/// The template under test: the native2026 edition's, as a first launch gets it.
[[nodiscard]] contour::config::IndicatorTemplate const& native2026()
{
    return contour::config::editionDefaults(contour::config::DefaultsEdition::Native2026).statusLine;
}

/// The page every case draws on: wide enough for each segment to be placed whole.
constexpr auto Page = PageSize { LineCount(5), ColumnCount(100) };

/// A terminal showing the native2026 template on the contour-dark scheme. Its filesystem is @p files, so
/// `{Git}` looks for a repository in an empty tree rather than on the machine the test runs on.
struct TemplateTerm
{
    explicit TemplateTerm(core::platform::FileSystem const& files): mc { Page, files }
    {
        REQUIRE(defaultColorPalettes("contour-dark", mc.terminal.defaultColorPalette()));
        mc.terminal.resetColorPalette();
        mc.terminal.setHistoryLimits(HistoryLimits::plain(MaxHistoryLineCount(LineCount(1000))));
        auto const& segments = native2026();
        mc.terminal.setStatusLineDefinition(
            parseStatusLineDefinition(segments.left, segments.middle, segments.right));
        mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
    }

    MockTerm<> mc;
};

/// Runs @p command at a fresh prompt for @p duration, ending it with @p exitCode and a new prompt.
void runFor(MockTerm<>& mc, std::string_view command, std::chrono::milliseconds duration, int exitCode)
{
    test::typeAtPrompt(mc, command);
    mc.writeToScreen("\033]133;C\033\\");
    mc.steadyClock.advance(duration);
    mc.writeToScreen(test::commandEnd(exitCode));
    mc.writeToScreen("\033]133;A\033\\$ ");
}

/// The status row's text, after drawing a frame.
[[nodiscard]] std::string statusRow(MockTerm<>& mc)
{
    REQUIRE(mc.terminal.refreshRenderBuffer());
    return mc.terminal.indicatorStatusLineDisplay().grid().lineText(LineOffset(0));
}

/// The attributes the status row's cell @p column is drawn with, after drawing a frame.
[[nodiscard]] RenderAttributes statusCellAt(MockTerm<>& mc, int column)
{
    REQUIRE(mc.terminal.refreshRenderBuffer());
    auto const buffer = mc.terminal.renderBuffer();
    REQUIRE(buffer.get().statusBar.has_value());
    auto const location =
        CellLocation { .line = buffer.get().statusBar->lineOffset, .column = ColumnOffset(column) };
    for (auto const& cell: buffer.get().cells)
        if (cell.position == location)
            return cell.attributes;
    return RenderAttributes { .backgroundColor = test::backgroundAt(buffer.get(), location) };
}

/// The placeholder names of @p segment's items, in order.
[[nodiscard]] std::vector<std::string_view> namesOf(StatusLineSegment const& segment)
{
    auto names = std::vector<std::string_view> {};
    for (auto const& item: segment)
        names.push_back(std::visit(
            [](auto const& v) { return StatusLineDefinitions::ItemTraits<std::decay_t<decltype(v)>>::Name; },
            item));
    return names;
}

} // namespace

TEST_CASE("IndicatorTemplate.isTheApprovedOne", "[config][statusline]")
{
    auto const& segments = native2026();
    auto const definition = parseStatusLineDefinition(segments.left, segments.middle, segments.right);

    CHECK(namesOf(definition.left) == std::vector { "InputMode"sv, "Context"sv, "Cwd"sv, "Git"sv });
    // {Selection} is the SelectionSize item.
    CHECK(namesOf(definition.middle) == std::vector { "LastCommand"sv, "Selection"sv, "SearchPrompt"sv });
    REQUIRE(definition.middle.size() == 3);
    CHECK(std::holds_alternative<StatusLineDefinitions::SelectionSize>(definition.middle[1]));
    CHECK(namesOf(definition.right) == std::vector { "Progress"sv, "Scroll"sv, "Clock"sv });

    REQUIRE(!definition.left.empty());
    auto const* const inputMode = std::get_if<StatusLineDefinitions::InputMode>(&definition.left.front());
    REQUIRE(inputMode != nullptr);
    CHECK(inputMode->look == StatusLineDefinitions::InputModeLook::Chip);
    CHECK(inputMode->hiddenIn == ViMode::Insert);

    for (auto const* const segment: { &definition.left, &definition.middle, &definition.right })
        for (auto const& item: *segment)
        {
            // Roles only, so every colour follows the scheme, light or dark.
            std::visit(
                [](auto const& v) {
                    INFO(StatusLineDefinitions::ItemTraits<std::decay_t<decltype(v)>>::Name);
                    auto const foregroundIsRgb =
                        v.foregroundColor.has_value() && std::holds_alternative<RGBColor>(*v.foregroundColor);
                    CHECK_FALSE(foregroundIsRgb);
                    auto const backgroundIsRgb =
                        v.backgroundColor.has_value() && std::holds_alternative<RGBColor>(*v.backgroundColor);
                    CHECK_FALSE(backgroundIsRgb);
                },
                item);
            // The chrome already shows both.
            CHECK_FALSE(std::holds_alternative<StatusLineDefinitions::Tabs>(item));
            CHECK_FALSE(std::holds_alternative<StatusLineDefinitions::Title>(item));
        }

    // Each segment is written as the serializer writes it, so one Apply in the settings page leaves a
    // first-launch contour.yml byte for byte as it was.
    for (auto const text: { segments.left, segments.middle, segments.right })
    {
        INFO(text);
        CHECK(serializeStatusLineSegment(parseStatusLineSegment(text)) == text);
    }
}

TEST_CASE("IndicatorTemplate.drawsTheCanvasStates", "[config][statusline]")
{
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto term = TemplateTerm { files };
    auto& mc = term.mc;

    mc.writeToScreen("\033]7;file:///srv/app\033\\");
    runFor(mc, "ctest", 134s, 0);
    mc.wallClock.advance(14h + 2min);

    // Insert mode: the chip hides, so the line opens on the directory.
    auto row = statusRow(mc);
    INFO(row);
    CHECK(row.starts_with(" /srv/app "));
    CHECK_FALSE(row.contains("INSERT"));
    auto constexpr Finished = "✓ ctest · 2m 14s"sv;
    auto const middle = row.find(Finished);
    REQUIRE(middle != std::string::npos);
    // Everything before it is ASCII, so its byte offset is its column.
    auto const finishedColumns = std::size_t { 16 };
    CHECK(middle == (unbox<std::size_t>(Page.columns) - finishedColumns) / 2);
    CHECK(row.ends_with("14:02 "));

    runFor(mc, "ctest -R gui", 12'400ms, 8);
    row = statusRow(mc);
    INFO(row);
    CHECK(row.contains("✗ exit 8 ctest -R gui · 12s"));

    for (auto const i: std::views::iota(0, 200))
        mc.writeToScreen(std::format("line {}\r\n", i));
    mc.terminal.inputHandler().setMode(ViMode::Normal);
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(40)));
    row = statusRow(mc);
    INFO(row);
    CHECK(row.starts_with(" NORMAL  /srv/app"));
    CHECK(row.contains("↑ 40 / "));
    CHECK(row.ends_with("14:02 "));
}

TEST_CASE("IndicatorTemplate.eachPaneDrawsItsOwnLine", "[config][statusline]")
{
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto a = TemplateTerm { files };
    auto b = TemplateTerm { files };

    a.mc.writeToScreen("\033]7;file:///srv/a\033\\");
    a.mc.terminal.inputHandler().setMode(ViMode::Normal);

    b.mc.writeToScreen("\033]7;file:///srv/b\033\\");
    runFor(b.mc, "make", 4ms, 0);
    // On contour-dark the inactive bar is the Insert one; B's own makes the focus change visible.
    auto& bScheme = b.mc.terminal.defaultColorPalette();
    bScheme.indicatorStatusLineInactive = RGBColorPair { .foreground = RGBColor { 0xC0, 0xC0, 0xC0 },
                                                         .background = RGBColor { 0x00, 0x00, 0x00 } };
    REQUIRE(bScheme.indicatorStatusLineInactive.background
            != bScheme.indicatorStatusLineInsertMode.background);
    b.mc.terminal.resetColorPalette();
    (void) b.mc.terminal.sendFocusOutEvent();

    auto const aRow = statusRow(a.mc);
    auto const bRow = statusRow(b.mc);
    INFO("A: " << aRow);
    INFO("B: " << bRow);
    CHECK(aRow.contains("NORMAL"));
    CHECK(aRow.contains("/srv/a"));
    CHECK_FALSE(aRow.contains("/srv/b"));
    CHECK(bRow.contains("/srv/b"));
    CHECK(bRow.contains("✓ make"));
    CHECK_FALSE(bRow.contains("NORMAL"));
    CHECK(statusCellAt(b.mc, 0).backgroundColor == bScheme.indicatorStatusLineInactive.background);

    // A role resolves against its own pane's scheme: B's success colour changes, A's does not.
    auto constexpr Success = RGBColor { 0x00, 0xFF, 0x80 };
    bScheme.palette[2] = Success;
    b.mc.terminal.resetColorPalette();
    // " /srv/b": the directory starts in column 1.
    CHECK(statusCellAt(b.mc, 1).foregroundColor == Success);
    // " NORMAL  /srv/a": A's directory starts in column 9.
    CHECK(statusCellAt(a.mc, 9).foregroundColor != Success);

    (void) b.mc.terminal.sendFocusInEvent();
    CHECK(statusCellAt(b.mc, 0).backgroundColor == bScheme.indicatorStatusLineInsertMode.background);
}

TEST_CASE("IndicatorTemplate.theShippedTemplatesCollapseCleanly", "[config][statusline][collapse]")
{
    // At every width: the mode is shown whenever the line holds it, every piece drawn is whole and in its
    // place, and no separator is left without its item.
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto const widths = std::views::iota(6, 121);

    // A terminal @p width columns wide, in vi Normal mode, drawing @p segments.
    auto const drawn =
        [&](int width, contour::config::IndicatorTemplate const& segments, auto const& prepare) {
            auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(width) }, files };
            prepare(mc);
            mc.terminal.setStatusLineDefinition(
                parseStatusLineDefinition(segments.left, segments.middle, segments.right));
            mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
            mc.terminal.inputHandler().setMode(ViMode::Normal);
            auto const row = statusRow(mc);
            auto const clock =
                serializeToVT(mc.terminal, parseStatusLineSegment("{Clock}"sv), StatusLineStyling::Disabled);
            auto const history = serializeToVT(
                mc.terminal, parseStatusLineSegment("{HistoryLineCount}"sv), StatusLineStyling::Disabled);
            auto const lastCommand = serializeToVT(
                mc.terminal, parseStatusLineSegment("{LastCommand}"sv), StatusLineStyling::Disabled);
            return std::array { row, clock, history, lastCommand };
        };

    SECTION("Classic")
    {
        for (auto const width: widths)
        {
            INFO(width);
            auto const [line, clock, history, _] =
                drawn(width, contour::config::ClassicIndicatorTemplate, [](MockTerm<>& mc) {
                    mc.writeToScreen("\033]2;title\033\\");
                });
            INFO(line);

            if (width >= 7)
                CHECK(line.starts_with(" NORMAL"));
            CHECK(line.contains("title") == line.contains("«"));
            CHECK(line.contains("title") == line.contains("»"));
            if (line.contains("title"))
                CHECK(line.contains("« title »"));
            CHECK(line.contains("│") == line.contains(':'));
            if (line.contains(':'))
                CHECK(line.ends_with(std::format("{} │ {}", history, clock)));
            if (width >= 26)
                CHECK(line.contains("« title »"));
        }
    }

    SECTION("Native2026")
    {
        for (auto const width: widths)
        {
            INFO(width);
            auto const [line, clock, _, lastCommand] = drawn(width, native2026(), [](MockTerm<>& mc) {
                mc.writeToScreen("\033]7;file:///srv/app\033\\");
                runFor(mc, "make", 4ms, 0);
            });
            INFO(line);
            REQUIRE(lastCommand.contains("make"));

            if (width >= 8)
                CHECK(line.starts_with(" NORMAL "));
            if (line.contains("srv"))
                CHECK(line.substr(8).starts_with(" /srv/app"));
            if (line.contains("make"))
                CHECK(line.contains(lastCommand));
            if (line.contains(':'))
                CHECK(line.ends_with(clock + " "));
            if (width >= 60)
            {
                CHECK(line.contains("/srv/app"));
                CHECK(line.contains(lastCommand));
                CHECK(line.ends_with(clock + " "));
            }
        }
    }
}
