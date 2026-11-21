// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Settings.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/CursorShape.hpp>
#include <core/tui/HeadlessSurface.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/layout/Style.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{

/// A served guest's output that would reach past the grid on an ordinary session, and how to see it did.
struct Effect
{
    std::string_view what;
    std::string bytes;
    std::function<bool(MockTerm<>&)> happened;
};

/// A kitty graphics command placing a 2x2 red image at the cursor.
[[nodiscard]] std::string kittyImage()
{
    // 2x2 pixels of 32-bit RGBA, base64-encoded: four times FF 00 00 FF.
    return "\033_Ga=T,f=32,s=2,v=2;/wAA//8AAP//AAD//wAA/w==\033\\";
}

/// Every effect the served-session allowlist must drop, each with its probe.
[[nodiscard]] std::vector<Effect> effects()
{
    auto const imageAtHome = [](MockTerm<>& mock) {
        return mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment() != nullptr;
    };
    return {
        { "OSC 52 clipboard",
          "\033]52;c;aGVsbG8=\033\\",
          [](MockTerm<>& m) { return !m.clipboardData.empty(); } },
        { "OSC 9 notification",
          "\033]9;hello\033\\",
          [](MockTerm<>& m) { return !m.notifications.empty(); } },
        { "OSC 2 title", "\033]2;title\033\\", [](MockTerm<>& m) { return m.windowTitle == "title"; } },
        { "sixel image", "\033Pq#0;2;100;0;0#0~~~~~~-~~~~~~\033\\", imageAtHome },
        { "OSC 7 working directory",
          "\033]7;file://localhost/tmp\033\\",
          [](MockTerm<>& m) { return !m.terminal.currentWorkingDirectory().empty(); } },
        { "BEL", "\a", [](MockTerm<>& m) { return m.bells != 0; } },
        { "ENQ answerback", "\x05", [](MockTerm<>& m) { return !m.terminal.peekInput().empty(); } },
        { "DA1 reply", "\033[c", [](MockTerm<>& m) { return !m.terminal.peekInput().empty(); } },
        { "APC kitty graphics", kittyImage(), imageAtHome },
        { "soft font designation",
          "\033P1;0;0;10;0;0;20;0{ @@\033\\\033( @",
          [](MockTerm<>& m) {
              return m.terminal.currentScreen().cursor().charsets.drcsFont(CharsetTable::G0).has_value();
          } },
        { "SCS DEC special graphics",
          "\033(0",
          [](MockTerm<>& m) {
              m.writeToScreen("q");
              return m.terminal.currentScreen().cellTextAt(
                         { .line = LineOffset(0), .column = ColumnOffset(0) })
                     != "q";
          } },
    };
}

constexpr auto Size = PageSize { LineCount(6), ColumnCount(30) };

/// @param allowance What the terminal executes.
/// @return A terminal of Size that answers ENQ, and draws images in 10x20-pixel cells.
[[nodiscard]] std::unique_ptr<MockTerm<>> makeTerminal(SequenceAllowance allowance)
{
    auto settings = MockTerm<>::createSettings(Size, HistoryLimits::plain(LineCount(20)), 1024);
    settings.sequences = allowance;
    settings.answerback = "answer";
    auto mock = std::make_unique<MockTerm<>>(std::move(settings));
    mock->terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
    return mock;
}

} // namespace

TEST_CASE("a served session drops what is not on the allowlist", "[vtbackend][served]")
{
    for (auto const& effect: effects())
    {
        INFO(effect.what);
        auto const mock = makeTerminal(SequenceAllowance::ServedTab);
        mock->writeToScreen(effect.bytes);
        CHECK_FALSE(effect.happened(*mock));
    }

    // What the guest draws is all that is left: the grid reads as the same bytes without the dropped ones.
    auto const served = makeTerminal(SequenceAllowance::ServedTab);
    auto const stripped = makeTerminal(SequenceAllowance::ServedTab);
    auto all = std::string {};
    for (auto const& effect: effects())
        all += effect.bytes;
    served->writeToScreen(std::format("\033[1;31m"
                                      "one\033[m{}\r\n\033[2Ctwo\033[?25l\033[2;5Hx",
                                      all));
    stripped->writeToScreen("\033[1;31m"
                            "one\033[m\r\n\033[2Ctwo\033[?25l\033[2;5Hx");
    CHECK(served->terminal.primaryScreen().renderMainPageText()
          == stripped->terminal.primaryScreen().renderMainPageText());
    auto const page = served->terminal.primaryScreen().renderMainPageText();
    CHECK(page.starts_with("one "));
    CHECK(page.contains("\n  twx "));
    CHECK(served->terminal.peekInput().empty());
}

TEST_CASE("the same bytes act on an ordinary session", "[vtbackend][served]")
{
    for (auto const& effect: effects())
    {
        INFO(effect.what);
        auto const mock = makeTerminal(SequenceAllowance::Everything);
        mock->writeToScreen(effect.bytes);
        CHECK(effect.happened(*mock));
    }
}

TEST_CASE("a served session sets the modes that draw, and no other", "[vtbackend][served]")
{
    auto const served = makeTerminal(SequenceAllowance::ServedTab);
    // One DECSET naming a mode that draws and one that does not: the first is set, the second dropped.
    served->writeToScreen("\033[?1049;6h");
    CHECK(served->terminal.isModeEnabled(DECMode::ExtendedAltScreen));
    CHECK_FALSE(served->terminal.isModeEnabled(DECMode::Origin));
    served->writeToScreen("\033[?25l");
    CHECK_FALSE(served->terminal.isModeEnabled(DECMode::VisibleCursor));

    auto const ordinary = makeTerminal(SequenceAllowance::Everything);
    ordinary->writeToScreen("\033[?1049;6h");
    CHECK(ordinary->terminal.isModeEnabled(DECMode::Origin));
}

// {{{ A real core::tui frame

namespace
{

/// A component that draws what a dashboard draws: a rounded box, truecolor and bold text, an extended
/// underline in its own color, wide glyphs, and a focused cursor in a shape of its own.
class FrameProbe final: public core::tui::Component
{
  public:
    void render(core::tui::Canvas& canvas) override
    {
        using core::tui::RgbColor;
        auto border = core::tui::Style {};
        border.fg = RgbColor { .r = 0x20, .g = 0x80, .b = 0xC0 };
        canvas.drawBox(canvas.area(), core::tui::BorderStyle::Rounded, border);

        auto title = core::tui::Style {};
        title.fg = RgbColor { .r = 0xFF, .g = 0x66, .b = 0x00 };
        title.bg = RgbColor { .r = 0x10, .g = 0x20, .b = 0x30 };
        title.bold = true;
        canvas.putString(1, 2, "Agents", title);

        auto curly = core::tui::Style {};
        curly.underlineStyle = core::tui::UnderlineStyle::Curly;
        curly.underlineColor = RgbColor { .r = 0x01, .g = 0x02, .b = 0x03 };
        canvas.putString(2, 2, "curly", curly);

        auto italic = core::tui::Style {};
        italic.italic = true;
        canvas.putString(3, 2, "\xE6\xBC\xA2\xE5\xAD\x97 ok", italic);
        canvas.setCursor(4, 3);
    }

    [[nodiscard]] bool focusable() const override { return true; }
    [[nodiscard]] core::tui::CursorShape cursorShape() const override
    {
        return core::tui::CursorShape::SteadyBar;
    }
};

/// @return The bytes core::tui writes for one frame of FrameProbe, on a surface of Size.
[[nodiscard]] std::string coreTuiFrame()
{
    auto bytes = std::string {};
    auto surface = core::tui::HeadlessSurface { core::tui::Size { .width = unbox<int>(Size.columns),
                                                                  .height = unbox<int>(Size.lines) },
                                                [&bytes](std::string_view chunk) { bytes.append(chunk); } };
    auto screen = core::tui::Screen { surface, core::tui::ScreenConfig {} };
    auto probe = FrameProbe {};
    screen.root().addChild(probe, { .flex = core::tui::layout::Style { .grow = 1 } });
    screen.setFocus(&probe);
    screen.draw();
    return bytes;
}

/// @return Whether every cell of the two terminals' current pages reads and is drawn alike.
[[nodiscard]] bool sameCells(MockTerm<>& a, MockTerm<>& b)
{
    auto& left = a.terminal.currentScreen();
    auto& right = b.terminal.currentScreen();
    for (auto const line: std::views::iota(0, unbox<int>(Size.lines)))
        for (auto const column: std::views::iota(0, unbox<int>(Size.columns)))
        {
            auto const at = CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) };
            auto const l = left.at(at.line, at.column);
            auto const r = right.at(at.line, at.column);
            if (left.cellTextAt(at) != right.cellTextAt(at) || l.flags() != r.flags()
                || l.foregroundColor() != r.foregroundColor() || l.backgroundColor() != r.backgroundColor()
                || l.underlineColor() != r.underlineColor() || l.width() != r.width())
            {
                UNSCOPED_INFO("cell " << line << "," << column << " differs");
                return false;
            }
        }
    return true;
}

} // namespace

TEST_CASE("a served session draws a core::tui frame as an ordinary one does", "[vtbackend][served][tui]")
{
    auto const frame = coreTuiFrame();
    constexpr auto BeginBatch = "\033[?2026h"sv;
    auto const begin = frame.find(BeginBatch);
    REQUIRE(begin != std::string::npos);
    auto const head = std::string_view { frame }.substr(0, begin + BeginBatch.size());
    auto const tail = std::string_view { frame }.substr(head.size());

    auto const served = makeTerminal(SequenceAllowance::ServedTab);
    auto const ordinary = makeTerminal(SequenceAllowance::Everything);
    for (auto* const mock: { served.get(), ordinary.get() })
    {
        mock->writeToScreen(head);
        CHECK(mock->terminal.isModeEnabled(DECMode::BatchedRendering));
        mock->writeToScreen(tail);
        CHECK_FALSE(mock->terminal.isModeEnabled(DECMode::BatchedRendering));
    }

    CHECK(sameCells(*served, *ordinary));
    auto const page = served->terminal.currentScreen().renderMainPageText();
    CHECK(page.contains("\xE2\x95\xAD")); // the rounded corner
    CHECK(page.contains("Agents"));
    CHECK(
        served->terminal.currentScreen().at(LineOffset(1), ColumnOffset(2)).flags().contains(CellFlag::Bold));
    CHECK(served->terminal.currentScreen()
              .at(LineOffset(2), ColumnOffset(2))
              .flags()
              .contains(CellFlag::CurlyUnderlined));
    CHECK(served->terminal.cursorShape() == ordinary->terminal.cursorShape());
    CHECK(served->terminal.cursorShape() == CursorShape::Bar);
    CHECK(served->terminal.currentScreen().cursor().position
          == ordinary->terminal.currentScreen().cursor().position);
}

// }}}
