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
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/TestEnvironment.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <vtpty/ChannelPty.hpp>

#include <core/Environment.hpp>

#include <QtCore/QRectF>
#include <QtGui/QGuiApplication>
#include <QtGui/QInputMethod>
#include <QtGui/QInputMethodEvent>

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QTest>

using contour::test::DisplayHarness;
using contour::test::HarnessWindow;
using vtbackend::CellLocation;
using vtbackend::ColumnOffset;
using vtbackend::LineOffset;

namespace
{

/// What the display answers the platform input method for the cursor's rectangle.
/// @param h The harness whose display is asked.
/// @return The answer, as a rectangle.
[[nodiscard]] QRectF cursorRectangle(DisplayHarness const& h)
{
    return h.display->inputMethodQuery(Qt::ImCursorRectangle).toRectF();
}

/// The rectangle the display's own geometry gives cell @p cell, as the query must answer it.
/// @param h     The harness whose display is asked.
/// @param cell  The cell, in viewport coordinates.
/// @param width How many columns the cell covers.
/// @return The cell's rectangle in item-local logical coordinates.
[[nodiscard]] QRectF rectangleOf(DisplayHarness const& h, CellLocation cell, uint8_t width = 1)
{
    auto const metrics = h.display->gridMetrics();
    return contour::display::imeCursorRectangle(
        metrics.pageMargin, metrics.cellSize, cell, width, h.display->devicePixelRatio());
}

/// Sends the display an input method event composing @p preedit.
/// @param h       The harness whose display is composed on.
/// @param preedit What the input method is composing.
void compose(DisplayHarness const& h, QString const& preedit)
{
    auto event = QInputMethodEvent { preedit, {} };
    QCoreApplication::sendEvent(h.display.get(), &event);
}

/// Records what the display answers for the cursor's rectangle each time it announces that rectangle.
///
/// QQuickItem::updateInputMethod() reaches QInputMethod::update(), which announces each rectangle query; the
/// platform asks for the rectangle when it hears that, so the answer at that moment is the one it acts on.
class AnnouncedRectangles
{
  public:
    explicit AnnouncedRectangles(DisplayHarness const& h):
        _connection { QObject::connect(QGuiApplication::inputMethod(),
                                       &QInputMethod::cursorRectangleChanged,
                                       [this, &h] { _answers.push_back(cursorRectangle(h)); }) }
    {
    }
    AnnouncedRectangles(AnnouncedRectangles const&) = delete;
    AnnouncedRectangles(AnnouncedRectangles&&) = delete;
    AnnouncedRectangles& operator=(AnnouncedRectangles const&) = delete;
    AnnouncedRectangles& operator=(AnnouncedRectangles&&) = delete;
    ~AnnouncedRectangles() { QObject::disconnect(_connection); }

    /// What was announced since the last call, oldest first.
    [[nodiscard]] std::vector<QRectF> take() { return std::exchange(_answers, {}); }

  private:
    std::vector<QRectF> _answers;
    QMetaObject::Connection _connection;
};

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

TEST_CASE("TerminalDisplay.ime.theCandidateWindowFollowsAPreeditMovedLeft", "[contour][ime]")
{
    // Composing after a full line: the cursor is on the last cell, where にほ does not fit, so the preedit
    // is drawn four columns left of the edge -- and the candidate window belongs under its first cell.
    DisplayHarness const h { HarnessWindow::Unrendered };
    auto const columns = unbox<int>(h.session->terminal().pageSize().columns);
    h.feedAndSettle(std::string(static_cast<size_t>(columns), 'x'));

    compose(h, QStringLiteral("にほ"));
    CHECK(cursorRectangle(h)
          == rectangleOf(h, { .line = LineOffset(0), .column = ColumnOffset(columns - 4) }, 2));
}

TEST_CASE("TerminalDisplay.ime.aDoubleWidthLineAnchorsWhereItsCellIsDrawn", "[contour][ime]")
{
    // DECDWL draws cell N at columns 2N and 2N + 1, and its line holds half the page's cells.
    DisplayHarness const h { HarnessWindow::Unrendered };
    auto const columns = unbox<int>(h.session->terminal().pageSize().columns);

    SECTION("the cursor's cell")
    {
        h.feedAndSettle("\033#6abc");
        CHECK(cursorRectangle(h) == rectangleOf(h, { .line = LineOffset(0), .column = ColumnOffset(6) }, 2));
    }

    SECTION("a preedit moved left on the half-width line")
    {
        // The line's last cell is cell columns / 2 - 1; にほ takes four of its cells.
        h.feedAndSettle("\033#6" + std::string(static_cast<size_t>(columns / 2), 'x'));
        compose(h, QStringLiteral("にほ"));
        CHECK(
            cursorRectangle(h)
            == rectangleOf(h, { .line = LineOffset(0), .column = ColumnOffset(2 * ((columns / 2) - 4)) }, 4));
    }
}

TEST_CASE("TerminalDisplay.ime.aReboundDisplayForgetsTheLastSessionsCell", "[contour][ime]")
{
    // The cell answered last belongs to the session it was answered for. A display rebound to another
    // session whose cursor names no cell answers that session's first cell, not a cell of the last one.
    DisplayHarness h { HarnessWindow::Unrendered };
    h.feedAndSettle(historyOf(h, 4) + "abc");
    REQUIRE(cursorRectangle(h) != rectangleOf(h, { .line = LineOffset(0), .column = ColumnOffset(0) }));

    auto other = contour::test::makeTestSession(&h.testApp.app().sessionsManager(),
                                                std::make_unique<vtpty::ChannelPty>(vtbackend::PageSize {
                                                    vtbackend::LineCount(25), vtbackend::ColumnCount(80) }),
                                                h.testApp.app());
    h.display->setSession(other.get());
    auto& terminal = other->terminal();
    for ([[maybe_unused]] auto const line: std::views::iota(0, 40))
        terminal.writeToScreen("line\r\n"); // takes the terminal's lock itself
    {
        auto const lock = std::lock_guard { terminal };
        REQUIRE(terminal.viewport().scrollUp(vtbackend::LineCount(2)));
        REQUIRE_FALSE(terminal.isCursorInViewport());
    }
    auto const answer = cursorRectangle(h);

    h.display->setSession(h.session.get());
    other->terminate();
    QCoreApplication::processEvents();
    other.reset();

    CHECK(answer == rectangleOf(h, { .line = LineOffset(0), .column = ColumnOffset(0) }));
}

TEST_CASE("TerminalDisplay.ime.geometryAndPreeditChangesReannounceTheRectangle", "[contour][ime]")
{
    DisplayHarness h { HarnessWindow::Unrendered };
    h.feedAndSettle("abc");
    h.display->forceActiveFocus();
    REQUIRE(h.display->hasActiveFocus());
    auto announced = AnnouncedRectangles { h };
    auto const cursorCell = CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) };

    SECTION("the pane moves without resizing")
    {
        // Item-local, so the cell's rectangle is unchanged; announcing it is what lets the platform map it
        // to the item's new place.
        h.display->setX(h.display->x() + 37);
        CHECK(announced.take() == std::vector { rectangleOf(h, cursorCell) });
    }

    SECTION("the font size changes")
    {
        // Offscreen the new font is only staged -- there is no frame to apply it -- so this pins that the
        // seam announces; that the announced rectangle is the new cell's is a display case below.
        auto fonts = h.session->profile().fonts.value();
        fonts.size.pt += 4;
        h.display->setFonts(fonts);
        CHECK_FALSE(announced.take().empty());
    }

    SECTION("the preedit changes")
    {
        compose(h, QStringLiteral("にほん"));
        QCoreApplication::processEvents();
        CHECK(announced.take() == std::vector { rectangleOf(h, cursorCell, 2) });
    }
}

TEST_CASE("TerminalDisplay.ime.aFontChangeAnnouncesTheNewCellsRectangle", "[display][ime]")
{
    // A font applied to a live frame changes every cell's rectangle; what is announced must be the new one.
    if (!contour::test::displayTestsEnabled(core::LiveEnvironment {}))
    {
        WARN("Skipping display test: set CONTOUR_TEST_DISPLAY=1 with a real display");
        return;
    }
    DisplayHarness h { HarnessWindow::Shown };
    h.feedAndSettle("abc");
    h.display->forceActiveFocus();
    REQUIRE(h.display->hasActiveFocus());
    auto const cursorCell = CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) };
    auto const before = rectangleOf(h, cursorCell);
    auto announced = AnnouncedRectangles { h };

    auto fonts = h.session->profile().fonts.value();
    fonts.size.pt += 4;
    h.display->setFonts(fonts);
    REQUIRE(QTest::qWaitFor([&] {
        h.pump();
        return rectangleOf(h, cursorCell) != before;
    }));

    auto const answers = announced.take();
    REQUIRE_FALSE(answers.empty());
    CHECK(answers.back() == rectangleOf(h, cursorCell));
}
