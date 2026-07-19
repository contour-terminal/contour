// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/primitives.h>

#include <vtrasterizer/BoxDrawingRenderer.h>
#include <vtrasterizer/FontDescriptions.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <vector>

namespace vtrasterizer
{

/// This class is responsible for grouping the text to be rendered
/// into clusters of codepoints that share the same text style and color.
class TextClusterGrouper
{
  public:
    struct Events
    {
        virtual ~Events() = default;

        virtual void renderTextGroup(std::u32string_view codepoints,
                                     gsl::span<unsigned> clusters,
                                     vtbackend::CellLocation initialPenPosition,
                                     TextStyle style,
                                     vtbackend::RGBColor color,
                                     vtbackend::LineFlags flags,
                                     vtbackend::GlyphSizing const& sizing,
                                     uint8_t bidiLevel) = 0;

        virtual bool renderBoxDrawingCell(vtbackend::CellLocation position,
                                          char32_t codepoint,
                                          vtbackend::RGBColor foregroundColor,
                                          vtbackend::LineFlags flags,
                                          uint8_t bidiLevel) = 0;
    };

    explicit TextClusterGrouper(Events& events);

    /// Must be invoked before a new terminal frame is rendered.
    void beginFrame() noexcept;

    void forceGroupStart() noexcept { _forceUpdateInitialPenPosition = true; }
    void forceGroupEnd() { flushTextClusterGroup(); }

    /// Renders a given terminal's grid cell that has been
    /// transformed into a RenderCell.
    void renderCell(vtbackend::RenderCell const& cell);

    void renderCell(vtbackend::CellLocation position,
                    std::u32string_view graphemeCluster,
                    vtbackend::RGBColor foregroundColor,
                    TextStyle style,
                    vtbackend::LineFlags flags,
                    vtbackend::GlyphSizing const& sizing = {},
                    uint8_t bidiLevel = 0);

    void renderLine(std::u32string_view text,
                    vtbackend::LineOffset lineOffset,
                    vtbackend::RGBColor foregroundColor,
                    TextStyle style,
                    vtbackend::LineFlags flags);

    /// Must be invoked when rendering the terminal's text has finished for this frame.
    void endFrame();

  private:
    /// Puts a sequence of codepoints that belong to the same grid cell at @p _pos
    /// at the end of the currently filled line.
    void appendCellTextToClusterGroup(std::u32string_view codepoints,
                                      TextStyle style,
                                      vtbackend::RGBColor color,
                                      vtbackend::LineFlags flags,
                                      vtbackend::GlyphSizing const& sizing,
                                      uint8_t bidiLevel);

    void flushTextClusterGroup();

    void resetAndMovePenForward(vtbackend::ColumnOffset penIncrementInX) noexcept
    {
        _codepoints.clear();
        _clusters.clear();
        _columnCount = 0;
        _initialPenPosition.column += penIncrementInX;
    }

    Events& _events;

    // pen-start position of this text group
    vtbackend::CellLocation _initialPenPosition {};

    // uniform text style for this text group
    TextStyle _style = TextStyle::Invalid;

    // uniform text color for this text group
    vtbackend::RGBColor _color {};
    vtbackend::LineFlags _lineFlags = vtbackend::LineFlag::None;

    /// Resolved bidirectional level shared by every cell of this group. A change in it ends the
    /// group exactly as a colour change does: a shaping run may not straddle a direction change,
    /// because the shaper is told which way to lay the whole run out.
    uint8_t _bidiLevel = 0;

    // codepoints within this text group with
    // uniform unicode properties (script, language, direction).
    std::vector<char32_t> _codepoints;

    /// Cluster index for each codepoint: the group-relative COLUMN the codepoint's cell starts at.
    ///
    /// Columns, not cells appended -- a double-width character advances this by two, so the value
    /// doubles as the offset the renderer places the glyph at. Counting cells instead would put every
    /// glyph after a CJK or emoji character one column too far left.
    std::vector<unsigned> _clusters;

    /// Number of grid columns this group occupies so far; the cluster the next cell will be given.
    int _columnCount = 0;

    /// The scale the current group is being drawn at. Part of the group's identity: a cell drawn at
    /// a different size cannot share a shaping run with its neighbours.
    vtbackend::GlyphSizing _sizing {};

    bool _forceUpdateInitialPenPosition = false;
};

} // namespace vtrasterizer
