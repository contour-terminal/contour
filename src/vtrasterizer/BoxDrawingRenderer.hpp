// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>

#include <vtrasterizer/BoxDrawingStyle.hpp>
#include <vtrasterizer/FontDescriptions.hpp>
#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RenderTarget.hpp>
#include <vtrasterizer/TextureAtlas.hpp>

#include <crispy/Point.hpp>
#include <crispy/StrongHash.hpp>

namespace vtrasterizer
{

/// Takes care of rendering the text cursor.
class BoxDrawingRenderer: public Renderable
{
    friend class BoxDrawingRendererTest;

  public:
    /// @param gridMetrics The grid metrics the tiles are sized by.
    /// @param style How the glyphs whose look is configurable are drawn.
    BoxDrawingRenderer(GridMetrics const& gridMetrics, BoxDrawingStyle style);

    /// Replaces the style this renderer draws with.
    ///
    /// The narrow exception to "configuration at construction time": live configuration reload is the
    /// feature, and this renderer is a member of TextRenderer, which the Renderer keeps across a reload
    /// (@see Renderer::setBoxDrawingStyle). Tiles already in the atlas are not reused afterwards: the
    /// style is part of every tile's atlas key.
    /// @param style The new style.
    void setStyle(BoxDrawingStyle style) noexcept { _resolved = resolve(style); }

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;
    void clearCache() override;

    /// Whether @p codepoint is one this renderer may draw under SOME style.
    ///
    /// Style-independent on purpose, so a caller can ask without a renderer at hand: a codepoint whose
    /// style leaves it to the font (Braille under BrailleStyle::Font, the git-graph glyphs under
    /// BranchStyle::None) is still claimed here, and render() then declines it (@see leavesToFont()).
    /// @param codepoint The codepoint to ask about.
    /// @return Whether the codepoint is in this renderer's repertoire.
    [[nodiscard]] static bool renderable(char32_t codepoint) noexcept;

    /// Whether this renderer's style leaves @p codepoint to the font, so render() declines it.
    ///
    /// The one place that says which glyphs a style hands back to the text path.
    /// @param codepoint A codepoint renderable() claims.
    /// @return Whether the style leaves @p codepoint to the font.
    [[nodiscard]] bool leavesToFont(char32_t codepoint) const noexcept;

    /// Whether @p codepoint is one of Contour's own private-use glyphs: the gutter's fold column, its status
    /// marks and the block rail's (@see docs/vt-extensions/private-use-glyphs.md).
    [[nodiscard]] static constexpr bool isContourGlyph(char32_t codepoint) noexcept
    {
        return 0x10F000 <= codepoint && codepoint <= 0x10F011;
    }

    /// The line thickness @p codepoint's tile is drawn with on a row with @p flags: the cell's own, doubled
    /// on a DECDWL row, whose text is stretched to twice its width -- except for Contour's own glyphs, which
    /// a two-cell gutter draws wide for their size, so the fold column keeps one stroke weight in either
    /// width.
    /// @param codepoint The glyph.
    /// @param flags The row's line flags.
    /// @param lineThickness The cell's light line thickness.
    /// @return The thickness to draw the glyph's lines with.
    [[nodiscard]] static int tileLineThickness(char32_t codepoint,
                                               vtbackend::LineFlags flags,
                                               int lineThickness) noexcept;

    /// Renders boxdrawing character.
    ///
    /// @param codepoint     the boxdrawing character's codepoint.
    /// @return false if the style leaves @p codepoint to the font (@see leavesToFont()) or its tile could not
    ///         be made.
    [[nodiscard]] bool render(vtbackend::LineOffset line,
                              vtbackend::ColumnOffset column,
                              char32_t codepoint,
                              vtbackend::LineFlags flags,
                              vtbackend::RGBColor color);

    void inspect(std::ostream& output) const override;

  private:
    /// The style together with its lane of every tile's atlas key, computed once per style.
    struct ResolvedStyle
    {
        BoxDrawingStyle style;        ///< The style as configured.
        crispy::StrongHash atlasLane; ///< BoxDrawingStyle::atlasKey() in lanes of its own.
    };

    [[nodiscard]] static ResolvedStyle resolve(BoxDrawingStyle style) noexcept;

    AtlasTileAttributes const* getOrCreateCachedTileAttributes(char32_t codepoint,
                                                               vtbackend::LineFlags flags,
                                                               int subIndex = 0);

    using Renderable::createTileData;
    [[nodiscard]] std::optional<TextureAtlas::TileCreateData> createTileData(char32_t codepoint,
                                                                             vtbackend::LineFlags flags,
                                                                             atlas::TileLocation tileLocation,
                                                                             int subIndex);

    [[nodiscard]] std::optional<atlas::Buffer> buildBoxElements(char32_t codepoint,
                                                                ImageSize size,
                                                                int lineThickness,
                                                                size_t supersampling = 1) const;

    [[nodiscard]] std::optional<atlas::Buffer> buildElements(char32_t codepoint,
                                                             ImageSize size,
                                                             int lineThickness);

    ResolvedStyle _resolved;
};

} // namespace vtrasterizer
