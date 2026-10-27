// SPDX-License-Identifier: Apache-2.0
//
// Where a live display tells the platform input method to open its candidate window
// (Qt::ImCursorRectangle), and when it tells it the place has moved.
//
// Every case runs offscreen: the display's renderer exists from setSession() on, so the query is answered
// from the real grid and the real grid metrics without a frame ever being drawn. The window is shown but
// never rendered (HarnessWindow::Unrendered), which is what lets the display take active focus -- without
// it, QQuickItem::updateInputMethod() tells the platform nothing.

#include <contour/display/ImeQueryRect.hpp>
#include <contour/test/DisplayHarness.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <QtCore/QRectF>
#include <QtGui/QGuiApplication>
#include <QtGui/QInputMethod>
#include <QtGui/QInputMethodEvent>

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <ranges>
#include <string>

#include <QtTest/QSignalSpy>

using contour::test::DisplayHarness;
using contour::test::HarnessWindow;
using vtbackend::CellLocation;
using vtbackend::ColumnOffset;
using vtbackend::LineOffset;

namespace
{

/// The rectangle the display's own geometry gives cell @p cell, as the query must answer it.
/// @param h    The harness whose display is asked.
/// @param cell The cell, in viewport coordinates.
/// @return The cell's rectangle in item-local logical coordinates.
[[nodiscard]] QRectF rectangleOf(DisplayHarness const& h, CellLocation cell)
{
    auto const metrics = h.display->gridMetrics();
    return contour::display::imeCursorRectangle(
        metrics.pageMargin, metrics.cellSize, cell, 1, h.display->devicePixelRatio());
}

/// What the display answers the platform input method for the cursor's rectangle.
/// @param h The harness whose display is asked.
/// @return The answer, as a rectangle.
[[nodiscard]] QRectF cursorRectangle(DisplayHarness const& h)
{
    return h.display->inputMethodQuery(Qt::ImCursorRectangle).toRectF();
}

/// Scrolls the display's viewport into the history, so the cursor is no longer on screen.
/// @param h     The harness whose terminal scrolls.
/// @param lines How many lines up.
void scrollBack(DisplayHarness const& h, int lines)
{
    auto& terminal = h.session->terminal();
    auto const lock = std::lock_guard { terminal };
    REQUIRE(terminal.viewport().scrollUp(vtbackend::LineCount(lines)));
    REQUIRE_FALSE(terminal.isCursorInViewport());
}

/// Output that leaves @p count lines of history above the page.
/// @param h     The harness whose page size decides how many lines that takes.
/// @param count How many lines of history.
/// @return The output to feed.
[[nodiscard]] std::string historyOf(DisplayHarness const& h, int count)
{
    auto const lines = unbox<int>(h.session->terminal().pageSize().lines) + count;
    auto output = std::string {};
    for ([[maybe_unused]] auto const line: std::views::iota(0, lines))
        output += "line\r\n";
    return output;
}

} // namespace

TEST_CASE("TerminalDisplay.ime.theWrapPendingCursorAnchorsAtTheLastCell", "[contour][ime]")
{
    DisplayHarness const h { HarnessWindow::Unrendered };
    auto const columns = unbox<int>(h.session->terminal().pageSize().columns);

    // A full line leaves the cursor wrap-pending. The grid keeps it on the last cell and flags the pending
    // wrap, so the last cell is where the candidate window belongs -- not the next line, and not the
    // pane's origin.
    h.feedAndSettle(std::string(static_cast<size_t>(columns), 'x'));
    {
        auto& terminal = h.session->terminal();
        auto const lock = std::lock_guard { terminal };
        REQUIRE(terminal.currentScreen().cursor().wrapPending);
        REQUIRE(terminal.currentScreen().cursor().position
                == CellLocation { .line = LineOffset(0), .column = ColumnOffset(columns - 1) });
    }

    auto const answer = cursorRectangle(h);
    CHECK_FALSE(answer.isEmpty());
    CHECK(answer == rectangleOf(h, { .line = LineOffset(0), .column = ColumnOffset(columns - 1) }));
}

TEST_CASE("TerminalDisplay.ime.aCursorOffScreenKeepsTheLastRectangle", "[contour][ime]")
{
    DisplayHarness const h { HarnessWindow::Unrendered };
    h.feedAndSettle(historyOf(h, 4) + "abc");

    auto const lastGood = cursorRectangle(h);
    REQUIRE_FALSE(lastGood.isEmpty());

    // Scrolled back, the cursor names no cell on screen: the candidate window stays where it was rather
    // than jumping to the pane's origin.
    scrollBack(h, 2);
    CHECK(cursorRectangle(h) == lastGood);
}

TEST_CASE("TerminalDisplay.ime.beforeAnyAnswerTheFirstCellStandsIn", "[contour][ime]")
{
    DisplayHarness const h { HarnessWindow::Unrendered };
    h.feedAndSettle(historyOf(h, 4));
    scrollBack(h, 2);

    auto const answer = cursorRectangle(h);
    CHECK_FALSE(answer.isEmpty());
    CHECK(answer == rectangleOf(h, { .line = LineOffset(0), .column = ColumnOffset(0) }));
}

TEST_CASE("TerminalDisplay.ime.geometryAndFontChangesReannounceTheRectangle", "[contour][ime]")
{
    DisplayHarness h { HarnessWindow::Unrendered };
    h.display->forceActiveFocus();
    REQUIRE(h.display->hasActiveFocus());

    // QQuickItem::updateInputMethod() reaches QInputMethod::update(), which announces each rectangle query.
    auto const spy = QSignalSpy { QGuiApplication::inputMethod(), &QInputMethod::cursorRectangleChanged };
    auto const announcedBy = [&](auto&& change) {
        auto const before = spy.count();
        change();
        QCoreApplication::processEvents();
        return spy.count() - before;
    };

    SECTION("the pane moves without resizing")
    {
        CHECK(announcedBy([&] { h.display->setX(h.display->x() + 37); }) == 1);
    }

    SECTION("the font size changes")
    {
        CHECK(announcedBy([&] {
                  auto fonts = h.session->profile().fonts.value();
                  fonts.size.pt += 4;
                  h.display->setFonts(fonts);
              })
              >= 1);
    }

    SECTION("the preedit changes")
    {
        CHECK(announcedBy([&] {
                  auto event = QInputMethodEvent { QStringLiteral("にほん"), {} };
                  QCoreApplication::sendEvent(h.display.get(), &event);
              })
              == 1);
    }
}
