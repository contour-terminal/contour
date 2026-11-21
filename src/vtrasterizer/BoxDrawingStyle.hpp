// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtrasterizer
{

/// How the builtin box-drawing renderer draws the glyphs whose look is a matter of taste: rounded corners,
/// Contour's git-graph glyphs (U+F5D0..U+F60D) and Braille patterns (U+2800..U+28FF).
///
/// A plain value handed to BoxDrawingRenderer at construction, so two renderers may draw differently and
/// nothing about the look is process-wide.
struct BoxDrawingStyle
{
    /// The shape of a rounded corner (U+256D..U+2570, and the arcs of the git-graph glyphs).
    enum class ArcStyle : uint8_t
    {
        Elliptic,
        Round,
    };

    /// The look of the git-graph glyphs.
    struct GitDrawingsStyle
    {
        /// How a merge commit's circle is drawn.
        enum class MergeCommitStyle : uint8_t
        {
            Solid,
            Bullet,
        };

        /// The line the branches are drawn with; None leaves the glyphs to the font.
        enum class BranchStyle : uint8_t
        {
            None,
            Thin,
            Double,
            Thick,
        };

        ArcStyle arcStyle = ArcStyle::Round;                          ///< The shape of a branch's bend.
        BranchStyle branchStyle = BranchStyle::Thin;                  ///< The branch line.
        MergeCommitStyle mergeCommitStyle = MergeCommitStyle::Bullet; ///< The merge commit's circle.

        [[nodiscard]] constexpr bool operator==(GitDrawingsStyle const&) const noexcept = default;
    };

    /// How a Braille pattern's dots are drawn; Font leaves them to the font.
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

    ArcStyle arcStyle = ArcStyle::Round;              ///< The shape of the box-drawing rounded corners.
    GitDrawingsStyle gitDrawings {};                  ///< The look of the git-graph glyphs.
    BrailleStyle brailleStyle = BrailleStyle::Circle; ///< How Braille dots are drawn.

    /// The arc style the git-graph glyphs are actually drawn with.
    ///
    /// Elliptic arcs are drawn with a thin stroke only, so the git-graph arcs keep their own arc style only
    /// where the branches are thin, and are round otherwise. Independent of the box-drawing arcStyle.
    /// @return The git-graph glyphs' effective arc style.
    [[nodiscard]] constexpr ArcStyle effectiveGitArcStyle() const noexcept
    {
        if (gitDrawings.branchStyle == GitDrawingsStyle::BranchStyle::Thin)
            return gitDrawings.arcStyle;
        return ArcStyle::Round;
    }

    /// Every field that changes a pixel, packed into one word, one byte per field.
    ///
    /// Two lanes of every box-drawing tile's atlas key, so a tile drawn under one style is never found under
    /// another: the atlas outlives a style change, and two renderers may share one atlas. Every field is a
    /// uint8_t enum with a byte of its own, so no enumerator a field gains can reach into its neighbour.
    /// @return The packed style.
    [[nodiscard]] constexpr uint64_t atlasKey() const noexcept
    {
        // One byte per field: a field added to either struct changes its size and fails here. A field
        // that changes a pixel must be packed below, or two styles drawing differently share their tiles.
        static_assert(sizeof(GitDrawingsStyle) == 3, "pack the new git-graph field into atlasKey()");
        static_assert(sizeof(BoxDrawingStyle) == 2 + sizeof(GitDrawingsStyle),
                      "pack the new box-drawing field into atlasKey()");

        return static_cast<uint64_t>(arcStyle)                                //
               | (static_cast<uint64_t>(effectiveGitArcStyle()) << 8U)        //
               | (static_cast<uint64_t>(gitDrawings.branchStyle) << 16U)      //
               | (static_cast<uint64_t>(gitDrawings.mergeCommitStyle) << 24U) //
               | (static_cast<uint64_t>(brailleStyle) << 32U);
    }

    [[nodiscard]] constexpr bool operator==(BoxDrawingStyle const&) const noexcept = default;
};

} // namespace vtrasterizer
