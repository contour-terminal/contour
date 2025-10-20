// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>

#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextureAtlas.h>

#include <crispy/point.h>

namespace vtrasterizer
{

// TODO: I think I should cincerely rename this class to
// something more suitable. it's not about box-drawing alone anymore,
// but about manually rendering anything that needs to properly fit
// into the grid cell.
// - box drawing symbols
// - symbols for legacy computing
// - mathematical symbols

/// Takes care of rendering the text cursor.
class BoxDrawingRenderer: public Renderable
{
  public:
    enum class ArcStyle : uint8_t
    {
        Ellips,
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
    enum class BraileStyle : uint8_t
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

    /// Renders boxdrawing character.
    ///
    /// @param codepoint     the boxdrawing character's codepoint.
    [[nodiscard]] bool render(vtbackend::LineOffset line,
                              vtbackend::ColumnOffset column,
                              char32_t codepoint,
                              vtbackend::RGBColor color);

    void inspect(std::ostream& output) const override;

    static void setBraileStyle(BraileStyle newStyle);
    static void setGitDrawingsStyle(GitDrawingsStyle newStyle);
    static void setArcStyle(ArcStyle newStyle) { arcStyle = newStyle; }

  private:
    AtlasTileAttributes const* getOrCreateCachedTileAttributes(char32_t codepoint);

    using Renderable::createTileData;
    [[nodiscard]] std::optional<TextureAtlas::TileCreateData> createTileData(
        char32_t codepoint, atlas::TileLocation tileLocation);

    [[nodiscard]] static std::optional<atlas::Buffer> buildBoxElements(char32_t codepoint,
                                                                       ImageSize size,
                                                                       int lineThickness,
                                                                       size_t supersampling = 1);
    [[nodiscard]] std::optional<atlas::Buffer> buildElements(char32_t codepoint);

    static inline ArcStyle arcStyle = ArcStyle::Round;
    static inline ArcStyle gitArcStyle = ArcStyle::Round;
    static inline BraileStyle braileStyle = BraileStyle::Font;
};

} // namespace vtrasterizer
