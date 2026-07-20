// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>

#include <vtrasterizer/FontDescriptions.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextureAtlas.h>

#include <crispy/point.h>

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

    /// Renders boxdrawing character.
    ///
    /// @param codepoint     the boxdrawing character's codepoint.
    [[nodiscard]] bool render(vtbackend::LineOffset line,
                              vtbackend::ColumnOffset column,
                              char32_t codepoint,
                              vtbackend::LineFlags flags,
                              vtbackend::RGBColor color,
                              bool mirrored = false);

    void inspect(std::ostream& output) const override;

    static void setBrailleStyle(BrailleStyle newStyle);
    static void setGitDrawingsStyle(GitDrawingsStyle newStyle);
    static void setArcStyle(ArcStyle newStyle) { DefaultArcStyle = newStyle; }

  private:
    AtlasTileAttributes const* getOrCreateCachedTileAttributes(char32_t codepoint,
                                                               vtbackend::LineFlags flags,
                                                               int subIndex = 0,
                                                               bool mirrored = false);

    using Renderable::createTileData;
    [[nodiscard]] std::optional<TextureAtlas::TileCreateData> createTileData(char32_t codepoint,
                                                                             vtbackend::LineFlags flags,
                                                                             atlas::TileLocation tileLocation,
                                                                             int subIndex,
                                                                             bool mirrored);

    [[nodiscard]] static std::optional<atlas::Buffer> buildBoxElements(char32_t codepoint,
                                                                       ImageSize size,
                                                                       int lineThickness,
                                                                       size_t supersampling = 1,
                                                                       bool mirrored = false);

    [[nodiscard]] std::optional<atlas::Buffer> buildElements(char32_t codepoint,
                                                             ImageSize size,
                                                             int lineThickness);

    static inline ArcStyle DefaultArcStyle = ArcStyle::Round;
    static inline ArcStyle DefaultGitArcStyle = ArcStyle::Round;
    static inline BrailleStyle DefaultBrailleStyle = BrailleStyle::Font;
};

} // namespace vtrasterizer
