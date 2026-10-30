// SPDX-License-Identifier: Apache-2.0
//
// Display-gated pixel tests for the in-grid visuals of Step 3e (hint labels, the overlay scrollbar) and of
// Step 6.10 (the inline preedit). A REAL TerminalDisplay bound to a REAL session is driven through the RHI
// loop, and what these cases measure is the grabbed frame: colours sampled from the pixels at the places the
// display's own geometry names, never at hard-coded offsets. The chrome snapshots stub the terminal and the
// render-command tests never see a pixel, so this is where a label whose ink fades into its fill, a thumb
// drawn in the wrong place or a preedit that took the cursor's colours would be found.
//
// Like every [display] case these SKIP unless CONTOUR_TEST_DISPLAY=1 opts into a real windowing system
// (scripts/run-display-tests.sh runs them under a headless weston).

#include <contour/config/Actions.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/ScrollBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/DisplayHarness.hpp>
#include <contour/test/DisplayPixels.hpp>
#include <contour/test/QmlChromeStyle.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <core/Environment.hpp>
#include <core/Utils.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QRect>
#include <QtGui/QImage>
#include <QtGui/QInputMethodEvent>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <QtTest/QTest>

using namespace contour::test::pixel;
using contour::test::DisplayHarness;
using namespace std::string_view_literals;

namespace
{

/// Which of a cell's colours a palette entry is for.
enum class CellColorRole : uint8_t
{
    Foreground, ///< The cell's ink.
    Background, ///< The cell's fill.
};

/// Resolves a palette colour that may name the cell's own colours, as the terminal does for a hint label.
/// @param color   The colour a palette entry holds.
/// @param palette The palette whose defaults a cell-relative colour resolves to.
/// @param role    Which of the cell's colours @p color is for.
/// @return The colour it stands for.
[[nodiscard]] vtbackend::RGBColor resolvedColor(vtbackend::CellRGBColor const& color,
                                                vtbackend::ColorPalette const& palette,
                                                CellColorRole role)
{
    if (std::holds_alternative<vtbackend::RGBColor>(color))
        return std::get<vtbackend::RGBColor>(color);
    return role == CellColorRole::Foreground ? palette.defaultForeground : palette.defaultBackground;
}

/// Hides the cursor, which animates between frames and would read as a change the case did not cause.
void hideCursor(DisplayHarness const& h)
{
    h.feedAndSettle("\033[?25l"sv);
}

/// Whether @p inner lies within @p outer, both non-empty.
[[nodiscard]] bool covers(QRect const& outer, QRect const& inner) noexcept
{
    return !inner.isNull() && outer.contains(inner);
}

/// The text rows the hint case prints: a row of plain words, then two rows that each hold a URL, with blank
/// rows between and after.
constexpr auto HintRows = std::array { "plain words on a row"sv,
                                       "see https://example.com/first here"sv,
                                       ""sv,
                                       "and https://example.org/second ok"sv };
constexpr auto PlainRow = 0;  ///< The row of plain words.
constexpr auto BlankRow = 12; ///< A row nothing is printed on.

[[nodiscard]] std::string hintPage()
{
    auto page = std::string {};
    for (auto const row: HintRows)
    {
        page += row;
        page += "\r\n";
    }
    return page;
}

} // namespace

TEST_CASE(
    "display: hint labels sit over their cells at the text ratio, and the rest of the page keeps its own",
    "[display][hints]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    for (auto const& variant: allVariants())
    {
        INFO(nameOf(variant));
        auto const h = makeHarness(variant);
        h->display->forceActiveFocus();
        hideCursor(*h);
        h->feedAndSettle(hintPage());
        auto const before = frameOf(*h);

        REQUIRE((*h->session)(
            contour::actions::HintMode { .patterns = "url", .scope = vtbackend::HintScope::Visible }));
        auto const after = frameOf(*h);
        auto const g = geometryOf(*h);
        auto const& palette = h->session->terminal().colorPalette();
        auto const matches =
            core::locked(h->session->terminal(), [&] { return h->session->terminal().hintMatches(); });
        REQUIRE(matches.size() == 2);

        auto const labelBackground =
            resolvedColor(palette.hintLabel.background, palette, CellColorRole::Background);
        auto const labelForeground =
            resolvedColor(palette.hintLabel.foreground, palette, CellColorRole::Foreground);
        for (auto const& match: matches)
        {
            auto const row = unbox<int>(match.start.line);
            auto const first = unbox<int>(match.start.column);
            auto const last = first + static_cast<int>(match.label.size()) - 1;
            auto const region = g.cells(row, first, last);
            auto const pixels = regionColors(after.image, region);
            INFO("label \"" << match.label << "\" row " << row << " columns " << first << ".." << last
                            << " fill " << hexOf(pixels.ground) << " ink " << hexOf(pixels.ink));

            // The render buffer drew the label's characters on those very cells.
            for (auto const [index, character]: core::views::enumerate(match.label))
            {
                auto const cell = std::ranges::find_if(after.buffer.cells, [&](auto const& candidate) {
                    return candidate.position
                           == vtbackend::CellLocation {
                                  .line = match.start.line,
                                  .column = match.start.column + vtbackend::ColumnOffset::cast_from(index)
                              };
                });
                REQUIRE(cell != after.buffer.cells.end());
                CHECK(cell->codepoints == std::u32string(1, static_cast<char32_t>(character)));
            }

            // Over the cells, in the pair the palette names, and legible: measured from the pixels.
            CHECK(nearlyEqual(pixels.ground, labelBackground));
            CHECK(nearlyEqual(pixels.ink, labelForeground, 40));
            CHECK(vtbackend::contrastRatio(pixels.ink, pixels.ground) >= TextContrast);

            // The label replaced what was there: the same cells before hint mode were not its colours.
            CHECK_FALSE(nearlyEqual(regionColors(before.image, region).ground, labelBackground));

            // The match's own text is tinted, not blanked: a cell past the label, inside the URL.
            auto const bodyColumn = last + 2;
            REQUIRE(bodyColumn <= unbox<int>(match.end.column));
            CHECK(regionColors(after.image, g.cells(row, bodyColumn, bodyColumn)).ground
                  != regionColors(before.image, g.cells(row, bodyColumn, bodyColumn)).ground);

            // Where the row has nothing printed, nothing changed.
            auto const rest = g.cells(
                row, static_cast<int>(HintRows[static_cast<size_t>(row)].size()) + 1, g.pageColumns - 1);
            CHECK_FALSE(changedBounds(before.image, after.image, rest).isValid());
        }

        // A row nothing was printed on is as it was.
        CHECK_FALSE(
            changedBounds(before.image, after.image, g.cells(BlankRow, 0, g.pageColumns - 1)).isValid());

        // The words of no match are dimmed toward the page: their ink is nearer the ground than it was.
        auto const plainBefore =
            regionColors(before.image, g.cells(PlainRow, 0, HintRows[PlainRow].size() - 1));
        auto const plainAfter =
            regionColors(after.image, g.cells(PlainRow, 0, HintRows[PlainRow].size() - 1));
        INFO("plain ink before " << hexOf(plainBefore.ink) << " after " << hexOf(plainAfter.ink));
        if (palette.hintUnmatchedOpacity < 1.0f)
            CHECK(distanceBetween(plainAfter.ink, plainAfter.ground)
                  < distanceBetween(plainBefore.ink, plainBefore.ground));
    }
}

// ---------------------------------------------------------------------------------------------------------
// The overlay scrollbar.

namespace
{

/// A shell session's worth of output that leaves history and failure marks to draw: failing commands with
/// output of their own, each long enough that the page scrolls.
[[nodiscard]] std::string failingHistory()
{
    auto output = std::vector<std::string> {};
    for (auto const line: std::views::iota(0, 30))
        output.push_back(std::format("line {}", line));
    auto page = std::string {};
    for ([[maybe_unused]] auto const block: std::views::iota(0, 3))
        page += vtbackend::test::commandRun("bad", output, 2);
    return page;
}

/// The harness of @p variant with an overlay scrollbar over its history, which hides at the bottom of it, and
/// the marks @p marks drawn on it.
[[nodiscard]] std::unique_ptr<DisplayHarness> makeScrollbarHarness(Variant const& variant,
                                                                   vtbackend::ScrollbarMarkSources marks)
{
    return makeHarness(variant, [&](contour::config::Config& config) {
        auto& scrollbar = config.profiles.value().at("main").scrollbar.value();
        scrollbar.position = contour::config::ScrollBarPosition::Right;
        scrollbar.mode = contour::config::ScrollBarModePreference::Overlay;
        scrollbar.marks = marks;
    });
}

/// The application's SessionChrome over a harness's display, as TerminalPane.qml hosts it: the scrollbar is a
/// child of the chrome, and the chrome a child of the display, so the bar is drawn into the display's own
/// frame.
class ScrollbarOverlay
{
  public:
    /// @param h       The harness whose display the chrome covers.
    /// @param variant The chrome row and scheme the chrome's tokens resolve with.
    ScrollbarOverlay(DisplayHarness& h, Variant const& variant): _harness { h }
    {
        auto const setup = contour::test::ChromeSetup {
            .row = contour::config::resolveChromeStyle(variant.style, contour::config::HostPlatform::Other),
            .font = QGuiApplication::font(),
            .windowControls = contour::config::WindowControlStyle::Windows,
            .scheme = contour::config::chromeSchemeColorsOf(h.session->terminal().colorPalette()),
        };
        contour::test::installChromeStyle(_engine, setup);
        // What the chrome's deferred bell asks of the application: whether multimedia is ready.
        _engine.rootContext()->setContextProperty(QStringLiteral("terminalSessions"),
                                                  &h.testApp.app().sessionsManager());

        auto component = QQmlComponent { &_engine };
        component.setData("import QtQuick\nimport Contour.Ui\nSessionChrome {}\n",
                          QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/ScrollbarOverlayHost.qml")));
        while (component.status() == QQmlComponent::Loading)
            QCoreApplication::processEvents();
        INFO("chrome errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
        _chrome.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(
            { { QStringLiteral("session"), QVariant::fromValue(static_cast<QObject*>(h.session.get())) } })));
        REQUIRE(_chrome != nullptr);
        _chrome->setParentItem(h.display.get());
        _bar = _chrome->findChild<QQuickItem*>(QStringLiteral("verticalScrollBar"));
        REQUIRE(_bar != nullptr);
        QCoreApplication::processEvents();
    }

    /// The scrollbar item.
    [[nodiscard]] QQuickItem& bar() const noexcept { return *_bar; }

    /// The chrome item, to hide it for the frame that shows the page alone.
    [[nodiscard]] QQuickItem& chrome() const noexcept { return *_chrome; }

    /// Scrolls the viewport @p offset rows back and waits until the bar has followed it: shown (or gone, for
    /// the bottom) with its thumb in the colour it settles on.
    /// @param offset How many rows above the bottom the viewport's top line is.
    void scrollTo(int offset) const
    {
        _harness.session->setScrollOffset(offset);
        QCoreApplication::processEvents();
        auto const wanted = _bar->property("wanted").toBool();
        REQUIRE(QTest::qWaitFor([&] { return _bar->opacity() == (wanted ? 1.0 : 0.0); }));
        auto* thumb = _bar->findChild<QQuickItem*>(QStringLiteral("scrollbarThumb"));
        REQUIRE(thumb != nullptr);
        auto const settled = [&] {
            auto const target = _bar->property("thumbColor").value<QColor>();
            if (!_bar->property("glyphThumb").toBool())
                return thumb->property("color").value<QColor>() == target;
            auto const glyphs = thumb->findChildren<QQuickItem*>(QStringLiteral("scrollbarThumbGlyph"));
            return std::ranges::all_of(glyphs, [&](auto* glyph) {
                return qvariant_cast<QColor>(glyph->property("color")) == target;
            });
        };
        REQUIRE(QTest::qWaitFor(settled));
    }

    /// The pixels of @p item in the grabbed frame.
    [[nodiscard]] QRect pixelsOf(QQuickItem const& item) const
    {
        auto const scene = item.mapRectToScene(QRectF { 0, 0, item.width(), item.height() });
        auto const dpr = _harness.display->devicePixelRatio();
        return QRect { QPoint { qRound(scene.left() * dpr), qRound(scene.top() * dpr) },
                       QPoint { qRound(scene.right() * dpr) - 1, qRound(scene.bottom() * dpr) - 1 } };
    }

  private:
    DisplayHarness& _harness;
    QQmlEngine _engine;
    std::unique_ptr<QQuickItem> _chrome;
    QQuickItem* _bar = nullptr;
};

/// The grabbed frame and the one the same page makes without the scrollbar over it.
struct BarFrames
{
    Frame withBar;    ///< The page with the chrome's scrollbar.
    Frame withoutBar; ///< The page alone.
};

[[nodiscard]] BarFrames barFramesOf(DisplayHarness const& h, ScrollbarOverlay const& overlay)
{
    auto frames = BarFrames { .withBar = frameOf(h), .withoutBar = {} };
    overlay.chrome().setVisible(false);
    frames.withoutBar = frameOf(h);
    overlay.chrome().setVisible(true);
    return frames;
}

/// The first and last row of @p area, in the range of rows where @p frames differ.
/// @return The rows the bar changed, or nothing where it changed none.
[[nodiscard]] std::optional<std::pair<int, int>> changedRows(BarFrames const& frames, QRect const& area)
{
    auto const changed = changedBounds(frames.withBar.image, frames.withoutBar.image, area);
    if (!changed.isValid())
        return std::nullopt;
    return std::pair { changed.top(), changed.bottom() };
}

} // namespace

TEST_CASE("display: the overlay scrollbar's thumb sits in the right-hand column at the scroll position",
          "[display][scrollbar]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    for (auto const& variant: allVariants())
    {
        INFO(nameOf(variant));
        auto const h = makeScrollbarHarness(variant, vtbackend::ScrollbarMarkSources {});
        h->display->forceActiveFocus();
        hideCursor(*h);
        h->feedAndSettle(failingHistory());
        auto overlay = ScrollbarOverlay { *h, variant };
        auto const g = geometryOf(*h);
        auto const& session = *h->session;
        REQUIRE(QTest::qWaitFor([&] { return session.historyLineCount() > 0; }));

        auto const total = session.pageLineCount() + session.historyLineCount();
        auto const track = overlay.pixelsOf(overlay.bar());
        auto const handleHeight = static_cast<double>(track.height()) * session.pageLineCount() / total;
        REQUIRE(handleHeight > g.cellHeight);

        for (auto const offset: { session.historyLineCount(), session.historyLineCount() / 2 })
        {
            INFO("scrolled " << offset << " rows back");
            overlay.scrollTo(offset);
            auto const frames = barFramesOf(*h, overlay);
            auto const area =
                QRect { track.left(), 0, track.width(), pageAreaOf(*h, frames.withBar.image).height() };
            auto const rows = changedRows(frames, area);
            REQUIRE(rows.has_value());

            // Where the thumb belongs: its fraction of the track, from the session's own line counts.
            auto const expectedTop =
                track.top()
                + (static_cast<double>(session.historyLineCount() - offset) / total * track.height());
            // The glyph thumb of the Terminal row is whole cells, the native one a shape of the exact height.
            auto const slack = variant.style == contour::config::UiStyle::Terminal ? g.cellHeight : 3;
            INFO("changed rows " << rows->first << ".." << rows->second << " expected " << expectedTop
                                 << " + " << handleHeight);
            CHECK(std::abs(rows->first - expectedTop) <= slack);
            CHECK(std::abs((rows->second - rows->first + 1) - handleHeight) <= slack);

            // In the rightmost column, and nowhere left of it: the page past the strip is untouched.
            auto const changed = changedBounds(
                frames.withBar.image, frames.withoutBar.image, pageAreaOf(*h, frames.withBar.image));
            CHECK(covers(QRect { g.columnLeft(g.pageColumns - 1),
                                 0,
                                 frames.withBar.image.width(),
                                 frames.withBar.image.height() },
                         changed));

            // The thumb's ink stands off the page under it.
            auto const middle = QRect { track.left(), (rows->first + rows->second) / 2, track.width(), 1 };
            auto const thumb = regionColors(frames.withBar.image, middle);
            auto const page = regionColors(frames.withoutBar.image, middle).ground;
            INFO("thumb " << hexOf(thumb.ink) << " page " << hexOf(page));
            if (variant.style == contour::config::UiStyle::Terminal)
                CHECK(vtbackend::contrastRatio(thumb.ink, page) >= MarkContrast);
            else
                CHECK(thumb.ground != page); // a resting native thumb is a tint, exempt from the mark ratio
        }

        // At the bottom the overlay bar is gone, and the column shows the page's own cells.
        overlay.scrollTo(0);
        auto const bottom = barFramesOf(*h, overlay);
        CHECK_FALSE(
            changedRows(
                bottom,
                QRect { track.left(), 0, track.width(), pageAreaOf(*h, bottom.withBar.image).height() })
                .has_value());
    }
}

TEST_CASE("display: the scrollbar's tick marks show in their colours against the track",
          "[display][scrollbar]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    for (auto const& variant: allVariants())
    {
        INFO(nameOf(variant));
        auto const h = makeScrollbarHarness(
            variant, vtbackend::ScrollbarMarkSources { vtbackend::ScrollbarMarkSource::Failures });
        h->display->forceActiveFocus();
        hideCursor(*h);
        h->feedAndSettle(failingHistory());
        auto overlay = ScrollbarOverlay { *h, variant };
        auto const& session = *h->session;
        REQUIRE(QTest::qWaitFor(
            [&] { return session.historyLineCount() > 0 && !session.scrollbarMarks().isEmpty(); }));
        // At the top, the thumb is up there and the ticks below it are clear of it.
        overlay.scrollTo(session.historyLineCount());
        auto const frames = barFramesOf(*h, overlay);

        auto const layer = overlay.bar().findChild<QQuickItem*>(QStringLiteral("scrollbarMarkLayer"));
        REQUIRE(layer != nullptr);
        auto const track = overlay.pixelsOf(*layer);
        auto const handle =
            overlay.pixelsOf(*overlay.bar().findChild<QQuickItem*>(QStringLiteral("scrollbarThumb")));
        auto const colors = session.scrollbarMarkColors();

        auto checked = 0;
        for (auto const& entry: session.scrollbarMarks())
        {
            auto const mark = entry.toMap();
            auto const kind = mark.value(QStringLiteral("kind")).toInt();
            auto const y =
                track.top() + qRound(mark.value(QStringLiteral("position")).toDouble() * track.height());
            // A tick under the thumb is drawn beneath it.
            if (y >= handle.top() - 4 && y <= handle.bottom() + 4)
                continue;
            ++checked;
            auto const tick = QRect { track.left(), y, track.width(), 2 };
            auto const drawn = regionColors(frames.withBar.image, tick).ink;
            auto const page = regionColors(frames.withoutBar.image, tick).ground;
            auto const expected = colors.at(kind).value<QColor>();
            INFO("tick at " << y << " drawn " << hexOf(drawn) << " expected " << expected.name().toStdString()
                            << " page " << hexOf(page));
            CHECK(nearlyEqual(drawn,
                              vtbackend::RGBColor { static_cast<uint8_t>(expected.red()),
                                                    static_cast<uint8_t>(expected.green()),
                                                    static_cast<uint8_t>(expected.blue()) }));
            CHECK(vtbackend::contrastRatio(drawn, page) >= MarkContrast);
        }
        CHECK(checked >= 1);
    }
}

// ---------------------------------------------------------------------------------------------------------
// The inline preedit.

namespace
{

/// A cursor colour no scheme has: the case's proof that the preedit's cells are not painted with it.
constexpr auto LoudCursor = vtbackend::RGBColor { 0xFF, 0x00, 0xFF };

/// The harness of @p variant with the inline preedit, and a block cursor in a colour of its own.
[[nodiscard]] std::unique_ptr<DisplayHarness> makePreeditHarness(Variant const& variant)
{
    return makeHarness(variant, [](contour::config::Config& config) {
        auto& profile = config.profiles.value().at("main");
        profile.inputMethodEditorStyle.value() = vtbackend::PreeditStyle::Inline;
        auto& colors = std::get<contour::config::SimpleColorConfig>(profile.colors.value()).colors;
        colors.cursor.color = LoudCursor;
        colors.cursor.textOverrideColor = LoudCursor;
    });
}

/// Sends the display an input method event composing @p preedit.
void composePreedit(DisplayHarness const& h, QString const& preedit)
{
    auto event = QInputMethodEvent { preedit, {} };
    QCoreApplication::sendEvent(h.display.get(), &event);
    QCoreApplication::processEvents();
}

/// Grabs frames until the block cursor has arrived where it is going: while it animates, no cell is painted
/// in its colours, so a frame grabbed earlier cannot show where those colours land.
/// @param h The harness to grab from.
/// @return The frame with the cursor at rest.
[[nodiscard]] Frame frameWithCursorAtRest(DisplayHarness const& h)
{
    auto frame = frameOf(h);
    auto const atRest = [&] {
        frame = frameOf(h);
        return frame.buffer.cursor.has_value() && frame.buffer.cursor->animationProgress >= 1.0f;
    };
    REQUIRE(QTest::qWaitFor(atRest, std::chrono::seconds(5)));
    return frame;
}

/// How the rows of a cell region split into the preedit's underline and the rest.
struct UnderlineRows
{
    std::vector<int> rows; ///< The frame rows that are ink across the whole region's width.
    int runWidth = 0;      ///< How wide the underline is, in pixels.
};

/// Finds the underline in @p region: the rows whose pixels, all across, are the ink's.
[[nodiscard]] UnderlineRows underlineIn(QImage const& image, QRect const& region, vtbackend::RGBColor ink)
{
    auto result = UnderlineRows {};
    for (auto const y: std::views::iota(region.top(), region.bottom() + 1))
    {
        auto run = 0;
        auto longest = 0;
        for (auto const x: std::views::iota(region.left(), region.right() + 1))
        {
            run = nearlyEqual(rgbOf(image.pixel(x, y)), ink, 40) ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        if (longest >= region.width() * 9 / 10)
        {
            result.rows.push_back(y);
            result.runWidth = std::max(result.runWidth, longest);
        }
    }
    return result;
}

/// How many pixels of @p region are @p color, to within the tolerance of sampled pixels.
[[nodiscard]] int countPixelsNear(QImage const& image, QRect const& region, vtbackend::RGBColor color)
{
    auto count = 0;
    for (auto const y: std::views::iota(region.top(), region.bottom() + 1))
        for (auto const x: std::views::iota(region.left(), region.right() + 1))
            if (nearlyEqual(rgbOf(image.pixel(x, y)), color))
                ++count;
    return count;
}

/// What the line holds before the preedit is composed on it.
enum class LineBefore : uint8_t
{
    Prefix,    ///< The case's own few characters: the preedit fits where the cursor is.
    WholeLine, ///< The page's width in characters: the cursor is on the last cell, where the preedit cannot
               ///< fit.
};

/// One placement of the preedit.
struct PreeditCase
{
    std::string_view name;   ///< What the placement is.
    LineBefore before;       ///< How full the line is.
    std::string_view prefix; ///< The output of a LineBefore::Prefix line.
    QString composed;        ///< The preedit.
    int firstColumn;         ///< Where its first cell is drawn, in screen columns (negative: from the right).
    int columns;             ///< How many screen columns it covers.
    int cellColumns;         ///< How many screen columns one grid cell covers: two on a DECDWL line.
};

} // namespace

TEST_CASE("display: the inline preedit is drawn at the default colours with a one-point underline",
          "[display][preedit]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    auto const cases = std::array {
        PreeditCase { .name = "at the cursor",
                      .before = LineBefore::Prefix,
                      .prefix = "ab"sv,
                      .composed = QStringLiteral("WiQ"),
                      .firstColumn = 2,
                      .columns = 3,
                      .cellColumns = 1 },
        PreeditCase { .name = "shifted left at the line's end",
                      .before = LineBefore::WholeLine,
                      .prefix = ""sv,
                      .composed = QStringLiteral("WiQ"),
                      .firstColumn = -3,
                      .columns = 3,
                      .cellColumns = 1 },
        PreeditCase { .name = "doubled on a DECDWL line",
                      .before = LineBefore::Prefix,
                      .prefix = "\033#6ab"sv,
                      .composed = QStringLiteral("Wi"),
                      .firstColumn = 4,
                      .columns = 4,
                      .cellColumns = 2 },
    };
    for (auto const& variant: allVariants())
    {
        for (auto const& placement: cases)
        {
            INFO(nameOf(variant) << ", " << placement.name);
            auto const h = makePreeditHarness(variant);
            h->display->forceActiveFocus();
            REQUIRE(h->session->terminal().focused());
            auto const columns = unbox<int>(h->session->terminal().pageSize().columns);
            // A steady block cursor, so the cursor's own colours are on the cell it stands on.
            h->feedAndSettle("\033[2 q"sv);
            h->feedAndSettle(placement.before == LineBefore::WholeLine
                                 ? std::string(static_cast<size_t>(columns), 'x')
                                 : std::string { placement.prefix });
            auto const g = geometryOf(*h);
            auto const& palette = h->session->terminal().colorPalette();

            // Before the composition the cursor's cell is in the cursor's colours: what the preedit must not
            // take.
            auto const standing = frameWithCursorAtRest(*h);
            auto const row = g.cells(0, 0, g.pageColumns - 1);
            CHECK(countPixelsNear(standing.image, row, LoudCursor) > 0);

            composePreedit(*h, placement.composed);
            auto const frame = frameWithCursorAtRest(*h);

            auto const first =
                placement.firstColumn < 0 ? columns + placement.firstColumn : placement.firstColumn;
            auto const last = first + placement.columns - 1;
            auto const region = g.cells(0, first, last);
            auto const pixels = regionColors(frame.image, region);
            INFO("columns " << first << ".." << last << " ground " << hexOf(pixels.ground) << " ink "
                            << hexOf(pixels.ink));

            // In the terminal's default colours.
            CHECK(nearlyEqual(pixels.ground, palette.defaultBackground));
            CHECK(nearlyEqual(pixels.ink, palette.defaultForeground, 24));
            // It reads as the text around it, whatever the scheme's own default ratio is (solarized-light's
            // is 4.1:1): the preedit takes the default pair, it does not lift it.
            CHECK(vtbackend::contrastRatio(pixels.ink, pixels.ground)
                  >= vtbackend::contrastRatio(palette.defaultForeground, palette.defaultBackground) - 0.3);

            // No cursor colours on its cells: not in the buffer's cells, not in their pixels.
            auto cellsChecked = 0;
            for (auto const& cell: frame.buffer.cells)
            {
                if (cell.position.line != vtbackend::LineOffset(0) || unbox<int>(cell.position.column) < first
                    || unbox<int>(cell.position.column) > last)
                    continue;
                ++cellsChecked;
                CHECK(cell.attributes.foregroundColor == palette.defaultForeground);
                CHECK(cell.attributes.backgroundColor == palette.defaultBackground);
            }
            CHECK(cellsChecked >= placement.composed.size());
            auto const cursorPixels = countPixelsNear(frame.image, region, LoudCursor);
            CHECK(cursorPixels == 0);

            // One logical point of underline, across the whole preedit and in its lower half.
            auto const underline = underlineIn(frame.image, region, palette.defaultForeground);
            auto const thickness = g.metrics.preeditUnderlineThickness;
            INFO("underline rows " << underline.rows.size() << " expected " << thickness);
            REQUIRE_FALSE(underline.rows.empty());
            CHECK(std::cmp_equal(underline.rows.size(), thickness));
            CHECK(underline.rows.front() > region.top() + (g.cellHeight / 2));
            CHECK(underline.runWidth >= region.width() * 9 / 10);

            // Confined to the preedit: the cell before it has no underline.
            if (first > 0)
                CHECK(underlineIn(frame.image, g.cells(0, first - 1, first - 1), palette.defaultForeground)
                          .rows.empty());
        }
    }
}
