// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>

#include <vtrasterizer/FontDescriptions.hpp>
#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RenderTarget.hpp>
#include <vtrasterizer/TextureAtlas.hpp>

#include <crispy/Point.hpp>

namespace vtrasterizer
{

/// Takes care of rendering the text cursor.
class BoxDrawingRenderer: public Renderable
{
    friend class BoxDrawingRendererTest;

  public:
    enum class ArcStyle : uint8_t
    {
        Elliptic,
        Round,
    };

    struct GitDrawingsStyle
    {
        enum class MergeCommitStyle : uint8_t
        {
            Solid,
            Bullet,
        };
        enum class BranchStyle : uint8_t
        {
            None,
            Thin,
            Double,
            Thick,
        };
        ArcStyle arcStyle = ArcStyle::Round;
        BranchStyle branchStyle = BranchStyle::Thin;
        MergeCommitStyle mergeCommitStyle = MergeCommitStyle::Bullet;
    };
    enum class BrailleStyle : uint8_t
    {
        Font,
        Solid,
        Circle,
        CircleEmpty,
        Square,
        SquareEmpty,
        AASquare,
        AASquareEmpty,
    };

    explicit BoxDrawingRenderer(GridMetrics const& gridMetrics): Renderable { gridMetrics } {}

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;
    void clearCache() override;

    [[nodiscard]] static bool renderable(char32_t codepoint) noexcept;

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
    [[nodiscard]] bool render(vtbackend::LineOffset line,
                              vtbackend::ColumnOffset column,
                              char32_t codepoint,
                              vtbackend::LineFlags flags,
                              vtbackend::RGBColor color);

    void inspect(std::ostream& output) const override;

    static void setBrailleStyle(BrailleStyle newStyle);
    static void setGitDrawingsStyle(GitDrawingsStyle newStyle);
    static void setArcStyle(ArcStyle newStyle) { DefaultArcStyle = newStyle; }

  private:
    AtlasTileAttributes const* getOrCreateCachedTileAttributes(char32_t codepoint,
                                                               vtbackend::LineFlags flags,
                                                               int subIndex = 0);

    using Renderable::createTileData;
    [[nodiscard]] std::optional<TextureAtlas::TileCreateData> createTileData(char32_t codepoint,
                                                                             vtbackend::LineFlags flags,
                                                                             atlas::TileLocation tileLocation,
                                                                             int subIndex);

    [[nodiscard]] static std::optional<atlas::Buffer> buildBoxElements(char32_t codepoint,
                                                                       ImageSize size,
                                                                       int lineThickness,
                                                                       size_t supersampling = 1);

    [[nodiscard]] std::optional<atlas::Buffer> buildElements(char32_t codepoint,
                                                             ImageSize size,
                                                             int lineThickness);

    static inline ArcStyle DefaultArcStyle = ArcStyle::Round;
    static inline ArcStyle DefaultGitArcStyle = ArcStyle::Round;
    static inline BrailleStyle DefaultBrailleStyle = BrailleStyle::Font;
};

} // namespace vtrasterizer
