// SPDX-License-Identifier: Apache-2.0
#include <contour/display/Logging.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/platform/QtInvoke.hpp>
#include <contour/session/FontControl.hpp>
#include <contour/session/Logging.hpp>
#include <contour/session/SizeIndicator.hpp>
#include <contour/session/TerminalSession.hpp>

#include <vtbackend/screen/Terminal.hpp>

#include <vtrasterizer/Renderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <ranges>

using std::string;

using vtbackend::ImageSize;

namespace contour::session
{

vtbackend::FontDef getFontDefinition(vtrasterizer::Renderer& renderer)
{
    // fontDescriptions() returns a mutex-guarded snapshot by value; take it once so all the reads below
    // observe a single consistent set of descriptions (and so the by-reference helpers below do not bind
    // to a temporary).
    auto const fonts = renderer.fontDescriptions();
    auto const fontByStyle = [&](text::FontWeight weight,
                                 text::FontSlant slant) -> text::FontDescription const& {
        auto const bold = weight != text::FontWeight::Normal;
        auto const italic = slant != text::FontSlant::Normal;
        if (bold && italic)
            return fonts.boldItalic;
        else if (bold)
            return fonts.bold;
        else if (italic)
            return fonts.italic;
        else
            return fonts.regular;
    };
    auto const nameOfStyledFont = [&](text::FontWeight weight, text::FontSlant slant) -> string {
        auto const& regularFont = fonts.regular;
        auto const& styledFont = fontByStyle(weight, slant);
        if (styledFont.familyName == regularFont.familyName)
            return "auto";
        else
            return styledFont.toPattern();
    };
    return { .size = fonts.size.pt,
             .regular = fonts.regular.familyName,
             .bold = nameOfStyledFont(text::FontWeight::Bold, text::FontSlant::Normal),
             .italic = nameOfStyledFont(text::FontWeight::Normal, text::FontSlant::Italic),
             .boldItalic = nameOfStyledFont(text::FontWeight::Bold, text::FontSlant::Italic),
             .emoji = fonts.emoji.toPattern() };
}

vtrasterizer::FontDescriptions sanitizeFontDescription(vtrasterizer::FontDescriptions fonts, text::DPI dpi)
{
    if (fonts.dpi.x <= 0 || fonts.dpi.y <= 0)
        fonts.dpi = dpi;
    if (std::fabs(fonts.size.pt) <= std::numeric_limits<double>::epsilon())
        fonts.size.pt = 12;
    return fonts;
}

bool applyFontDescription(text::DPI dpi,
                          vtrasterizer::Renderer& renderer,
                          vtrasterizer::FontDescriptions fontDescriptions)
{
    if (renderer.fontDescriptions() == fontDescriptions)
        return false;

    // setFonts() only *stages* the change; the shaper reconfiguration, font loading and
    // grid-metrics/atlas rebuild happen on the render thread in applyPendingReconfig(). Do NOT call
    // updateFontMetrics() here: it mutates _gridMetrics, rebuilds the texture atlas and touches the
    // non-thread-safe text shaper, which would race the render thread mid-frame (issue #1922).
    renderer.setFonts(sanitizeFontDescription(std::move(fontDescriptions), dpi));

    return true;
}

vtbackend::GutterLayout configuredGutterLayout(config::Config const& config, GutterChrome const& chrome)
{
    return vtbackend::gutterLayoutFor(config::gutterSettingsFor(config, chrome.form));
}

geometry::GutterExtent gutterExtentFor(vtbackend::GutterLayout const& layout,
                                       vtbackend::ImageSize cellSize,
                                       double scale,
                                       GutterChrome const& chrome)
{
    auto const columns = gutterWidthFor(layout, cellSize);
    auto const rail = gutterRailMetricsFor(layout, chrome, scale).strip;
    if (columns + rail == 0)
        return {};
    return geometry::GutterExtent { .leadIn = geometry::gutterLeadIn(scale),
                                    .columns = columns,
                                    .rail = rail };
}

vtrasterizer::BlockChipMetrics blockChipMetricsFor(GutterChrome const& chrome, double scale) noexcept
{
    auto metrics =
        vtrasterizer::BlockChipMetrics { .markEdge = chromeDevicePixels(chrome.markPoints, scale),
                                         .markFontSize = chrome.chips.markType,
                                         .markWeight = chrome.chips.markWeight,
                                         .hoverRing = chromeDevicePixels(chrome.chips.hoverRing, scale) };
    for (auto const index: std::views::iota(std::size_t { 0 }, vtbackend::PillStyleCount))
    {
        auto const& tokens = chrome.chips.pills[index];
        metrics.pills[index] = vtrasterizer::PillMetrics {
            .fontSize = tokens.type,
            .weight = tokens.weight,
            .padX = chromeDevicePixels(tokens.padX, scale),
            .padY = chromeDevicePixels(tokens.padY, scale),
            .height = chromeDevicePixels(tokens.height, scale),
            .radius = chromeDevicePixels(tokens.radius, scale),
            // A hairline: at least a pixel where there is one at all.
            .ring = tokens.ring > 0.0 ? std::max(1, static_cast<int>(std::lround(tokens.ring * scale))) : 0,
        };
    }
    return metrics;
}

vtrasterizer::GutterRailMetrics gutterRailMetricsFor(vtbackend::GutterLayout const& layout,
                                                     GutterChrome const& chrome,
                                                     double scale) noexcept
{
    // The chips are the native rows' wherever the block column is the rail, strip or none.
    auto metrics = vtrasterizer::GutterRailMetrics {};
    if (chrome.form == vtbackend::BlockColumnForm::Rail)
        metrics.chips = blockChipMetricsFor(chrome, scale);
    if (layout.rail == vtbackend::BlockRailPresence::Absent)
        return metrics;
    metrics.strip = chromeDevicePixels(chrome.stripPoints, scale);
    metrics.bar = chromeDevicePixels(chrome.barPoints, scale);
    metrics.barInset = chromeDevicePixels(chrome.barInsetPoints, scale);
    metrics.radius = chromeDevicePixels(chrome.barRadiusPoints, scale);
    metrics.endInset = chromeDevicePixels(chrome.barEndPoints, scale);
    return metrics;
}

geometry::GutterExtent configuredGutterExtent(config::Config const& config,
                                              vtbackend::ImageSize cellSize,
                                              double scale,
                                              GutterChrome const& chrome)
{
    return gutterExtentFor(configuredGutterLayout(config, chrome), cellSize, scale, chrome);
}

geometry::GutterWidth configuredGutterWidth(config::Config const& config,
                                            vtbackend::ImageSize cellSize,
                                            double scale,
                                            GutterChrome const& chrome)
{
    auto const extent = configuredGutterExtent(config, cellSize, scale, chrome);
    return extent.leadIn + extent.columns + extent.rail;
}

void announcePageSize(TerminalSession& session, vtbackend::PageSize oldPage, vtbackend::PageSize newPage)
{
    auto const linesChanged = oldPage.lines != newPage.lines;
    auto const columnsChanged = oldPage.columns != newPage.columns;
    if (linesChanged || columnsChanged)
        platform::postToObject(&session, [&session, newPage, linesChanged, columnsChanged]() {
            if (linesChanged)
                emit session.lineCountChanged(newPage.lines.as<int>());
            if (columnsChanged)
                emit session.columnsCountChanged(newPage.columns.as<int>());
        });
}

void applyResize(ImageSize newPixelSize, TerminalSession& session, vtrasterizer::Renderer& renderer)
{
    if (*newPixelSize.width == 0 || *newPixelSize.height == 0)
        return;

    vtbackend::Terminal& terminal = session.terminal();
    auto const oldPageSize = terminal.totalPageSize();
    // Read the published cell size once (lock-free), reused for both the page size and margin below.
    ImageSize const cellSize = renderer.publishedCellSize();
    // The device-pixel RATIO, not the content scale. This is a geometry conversion feeding the page fit,
    // and the surface only has `logical * dpr` hardware pixels to fit a page into. Under KDE's
    // forceFontDPI the content scale is the *font* decision and replaces the ratio, so scaling the margins
    // by it fits the page against a width the surface does not have — while TerminalDisplay bakes the very
    // same margin into the renderer using the DPR, leaving the two disagreeing. @see TerminalDisplay.h,
    // which names the page fit and the margins explicitly.
    auto const devicePixelRatio = session.display()->devicePixelRatio();
    auto const marginsDevicePx =
        geometry::scaled(toGeometryMargins(session.profile().margins.value()), devicePixelRatio);

    // Fit the grid with the terminal's own total-page clamp injected, so the renderer geometry published
    // below and what resizeScreen() will actually apply can never disagree: resizeScreen() raises the
    // total up to statusLineHeight()+1 lines and 1 column, and a fit computed against an unclamped page
    // would permanently defeat the early-out below two cell-rows (re-running resizeScreen() +
    // clearSelection() on every drag frame, wiping the selection and storming SIGWINCH at the child).
    auto const fit = geometry::fitPageToPixels(
        newPixelSize,
        cellSize,
        marginsDevicePx,
        [&](auto page) { return terminal.clampedTotalPageSize(page); },
        configuredGutterWidth(session.config(), cellSize, devicePixelRatio, session.gutterChrome()));

    // The strip the fit reserved, from the same layout, for the renderer to draw the rail in.
    renderer.applyResize(
        newPixelSize,
        fit.pageSize,
        fit.pageMargin,
        gutterRailMetricsFor(configuredGutterLayout(session.config(), session.gutterChrome()),
                             session.gutterChrome(),
                             devicePixelRatio));

    // This runs on the render thread too (TerminalDisplay::createRenderer()).
    announcePageSize(session, oldPageSize, fit.pageSize);

    // What the child is told, which is not the same question as how big the window is: the profile
    // decides whether the display's content scale is divided out first, and margins are excluded
    // because resizeScreen() divides this by the page to recover the cell size.
    auto const viewSize = session.display()->reportedPixelSize(fit.pageSize);
    display::displayLog()("Applying resize {}/{} pixels (margins {}) and {} -> {} cells.",
                          viewSize,
                          newPixelSize,
                          session.profile().margins.value(),
                          oldPageSize,
                          fit.pageSize);

    // fit.pageSize is already clamped (see above), so resizeToFit()'s direct comparison decides the
    // early-out.
    resizeToFit(session, fit.pageSize, viewSize);
}

void resizeToFit(TerminalSession& session, vtbackend::PageSize totalPageSize, ImageSize viewSize)
{
    auto& terminal = session.terminal();
    {
        // With in-band resize reports on (DECSET 2048) the resize writes a report, which with SRM reset is
        // echoed and parses, taking the lock again. The lock holds the report back and sends it once it is
        // released.
        auto const _ = std::scoped_lock { terminal };
        if (totalPageSize == terminal.totalPageSize())
        {
            display::displayLog()("No resize necessary. New size is same as old size of {}.", totalPageSize);
            return;
        }

        terminal.resizeScreen(totalPageSize, viewSize);
        terminal.clearSelection();
    }

    // Once per change, not per dimension. Posted: the indicator is GUI-thread state, and this runs on the
    // render thread when createRenderer() fits a window that lost its render target. Dispatched outside the
    // lock, it reads the newest main page; postToObject drops it if the session is gone by then.
    platform::postToObject(&session,
                           [&session]() { session.requestSizeIndicator(SizeIndicatorKind::PageSize); });
}

} // namespace contour::session
