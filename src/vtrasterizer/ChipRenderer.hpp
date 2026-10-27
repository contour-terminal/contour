// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The native rows' block chips and rail marks, as the canvas's `blocks` board draws them: rounded pills and
/// discs, their labels shaped in the platform's UI font through the grid's own shaper and atlas.

#include <vtbackend/core/BlockPill.hpp>
#include <vtbackend/core/BlockRailKinds.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/render/RenderBuffer.hpp>

#include <vtrasterizer/FontDescriptions.hpp>
#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RailGeometry.hpp>

#include <text_shaper/Font.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace vtrasterizer
{

class RenderTarget;
class TextRenderer;
struct FontKeys;

/// One rectangle of a pill, in device pixels, with its colour.
struct PillQuad
{
    int x = 0;                     ///< Left edge.
    int y = 0;                     ///< Top edge.
    int width = 0;                 ///< Width.
    int height = 0;                ///< Height.
    vtbackend::RGBAColor color {}; ///< Its colour, its alpha the coverage times the ink's.

    bool operator==(PillQuad const&) const = default;
};

/// The rectangles a rounded box is drawn as: the straight middle as one rectangle, and each pixel row across
/// a curved end as a span whose two edge pixels carry the circle's coverage in their alpha -- rectangles are
/// all the render target draws, and this is how they draw a curve. Then, where @p ring is set, the ring's
/// spans over it: the box less the box @p ringWidth inside it.
/// @param box Where the pill lies.
/// @param radius Its corners' radius, clamped to half its height and half its width.
/// @param fill Its fill; none draws no fill.
/// @param ring The ring's colour; none draws no ring.
/// @param ringWidth The ring's width.
/// @return The rectangles, the fill's before the ring's.
[[nodiscard]] std::vector<PillQuad> pillQuads(RailRect box,
                                              int radius,
                                              std::optional<vtbackend::RGBAColor> fill,
                                              std::optional<vtbackend::RGBAColor> ring,
                                              int ringWidth);

/// Where a pill of @p labelWidth pixels lies in its placed columns: as wide as the label and its padding
/// (never wider than the columns), at the edge its anchor names, vertically centred on its row, as tall as
/// its style's height or its label's line and padding, never taller than the row.
/// @param chip The chip.
/// @param pill Its style's geometry.
/// @param labelWidth Its label's shaped width.
/// @param labelLineHeight The UI font's line height.
/// @param metrics The grid's metrics.
/// @param smoothScrollOffset The rows' sub-cell scroll offset.
/// @return The box.
[[nodiscard]] RailRect pillBoxOf(vtbackend::RenderChip const& chip,
                                 PillMetrics const& pill,
                                 int labelWidth,
                                 int labelLineHeight,
                                 GridMetrics const& metrics,
                                 int smoothScrollOffset) noexcept;

/// The CSS weight @p weight as the font's: 400 regular, 600 semibold, 700 bold, and the nearest between.
/// @param weight A weight on the CSS 100-900 scale.
/// @return The font weight.
[[nodiscard]] text::FontWeight fontWeightOf(int weight) noexcept;

/// Draws the native rows' pills and marks, and measures their labels for the terminal's placement.
///
/// Its UI font is a second face in the grid's shaper, so its glyphs share the grid's atlas; the faces are
/// loaded on the render thread on first use and dropped with the grid's fonts (clearCache()). Holds the
/// renderer's own objects by reference, and lives exactly as long as they do.
class ChipRenderer
{
  public:
    /// @param gridMetrics The renderer's live metrics.
    /// @param textRenderer The grid's text renderer: its shaper and atlas draw the labels.
    /// @param fontDescriptions The grid's fonts: their size bounds the labels'.
    /// @param fontKeys The grid's faces: the regular one stands in where no UI font loads.
    ChipRenderer(GridMetrics const& gridMetrics,
                 TextRenderer& textRenderer,
                 FontDescriptions const& fontDescriptions,
                 FontKeys const& fontKeys);

    /// Forgets the UI faces and the measured labels: the grid's fonts, or its shaper, changed.
    void clearCache();

    /// The labels' advances in each style's face, measured once per font and chip metrics: what the terminal
    /// places a pill over whole columns by. Unmeasured where the row draws no pills.
    /// @return The metrics.
    [[nodiscard]] vtbackend::PillLabelMetrics const& labelMetrics();

    /// Draws @p chip: its pill, where its style has a fill or a ring, and its label over it.
    /// @param target Where the rectangles go.
    /// @param chip The chip.
    /// @param smoothScrollOffset The rows' sub-cell scroll offset.
    void renderChip(RenderTarget& target, vtbackend::RenderChip const& chip, int smoothScrollOffset);

    /// Draws @p cell's mark centred on the bar: its glyph in the UI font, on a disc where its face has one.
    /// @param target Where the rectangles go.
    /// @param cell The rail cell; its mark's face is not MarkShape::Cells.
    /// @param page The page's colour, which a disc's glyph is knocked out in.
    /// @param smoothScrollOffset The rows' sub-cell scroll offset.
    void renderMark(RenderTarget& target,
                    vtbackend::RenderGutterCell const& cell,
                    vtbackend::RGBColor page,
                    int smoothScrollOffset);

  private:
    /// The UI face at @p weight and @p sizePt, loaded once: the grid's regular face where none loads.
    [[nodiscard]] text::FontKey uiFont(int weight, double sizePt);

    GridMetrics const& _gridMetrics;
    TextRenderer& _textRenderer;
    FontDescriptions const& _fontDescriptions;
    FontKeys const& _fontKeys;

    /// The UI faces loaded so far, by weight and size in points.
    std::map<std::pair<int, double>, text::FontKey> _uiFonts;

    /// The labels measured, and the chip metrics and cell width they were measured for.
    vtbackend::PillLabelMetrics _labelMetrics {};
    std::optional<std::pair<BlockChipMetrics, int>> _measuredFor {};
};

} // namespace vtrasterizer
