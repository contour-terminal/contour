// SPDX-License-Identifier: Apache-2.0
#include <contour/display/Logging.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/session/FontControl.hpp>
#include <contour/session/Logging.hpp>
#include <contour/session/TerminalSession.hpp>

#include <vtbackend/Terminal.hpp>

#include <vtrasterizer/Renderer.hpp>

#include <algorithm>
#include <mutex>

using std::scoped_lock;
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

void applyResize(ImageSize newPixelSize, TerminalSession& session, vtrasterizer::Renderer& renderer)
{
    if (*newPixelSize.width == 0 || *newPixelSize.height == 0)
        return;

    vtbackend::Terminal& terminal = session.terminal();
    auto const oldPageSize = terminal.totalPageSize();
    // Read the published cell size once (lock-free), reused for both the page size and margin below.
    ImageSize const cellSize = renderer.publishedCellSize();
    auto const marginsDevicePx = geometry::scaled(toGeometryMargins(session.profile().margins.value()),
                                                  session.display()->contentScale());

    // Fit the grid with the terminal's own total-page clamp injected, so the renderer geometry published
    // below and what resizeScreen() will actually apply can never disagree: resizeScreen() raises the
    // total up to statusLineHeight()+1 lines and 1 column, and a fit computed against an unclamped page
    // would permanently defeat the early-out below two cell-rows (re-running resizeScreen() +
    // clearSelection() on every drag frame, wiping the selection and storming SIGWINCH at the child).
    auto const fit = geometry::fitPageToPixels(newPixelSize, cellSize, marginsDevicePx, [&](auto page) {
        return terminal.clampedTotalPageSize(page);
    });

    renderer.applyResize(newPixelSize, fit.pageSize, fit.pageMargin);

    if (oldPageSize.lines != fit.pageSize.lines)
        emit session.lineCountChanged(fit.pageSize.lines.as<int>());

    if (oldPageSize.columns != fit.pageSize.columns)
        emit session.columnsCountChanged(fit.pageSize.columns.as<int>());

    // What the child is told, which is not the same question as how big the window is: the profile
    // decides whether the display's content scale is divided out first, and margins are excluded
    // because resizeScreen() divides this by the page to recover the cell size.
    auto const viewSize = session.display()->reportedPixelSize(fit.pageSize);
    display::displayLog()("Applying resize {}/{} pixels (margins {}) and {} -> {} cells.",
                          viewSize,
                          newPixelSize,
                          session.profile().margins.value(),
                          terminal.pageSize(),
                          fit.pageSize);

    auto const l = scoped_lock { terminal };

    // fit.pageSize is already clamped (see above), so a direct comparison decides the early-out.
    if (fit.pageSize == terminal.totalPageSize())
    {
        display::displayLog()("No resize necessary. New size is same as old size of {}.", fit.pageSize);
        return;
    }

    terminal.resizeScreen(fit.pageSize, viewSize);
    terminal.clearSelection();
}

} // namespace contour::session
