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
                                     vtbackend::RGBColor color) = 0;

        virtual bool renderBoxDrawingCell(vtbackend::CellLocation position,
                                          char32_t codepoint,
                                          vtbackend::RGBColor foregroundColor) = 0;
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
                    TextStyle style,
                    vtbackend::RGBColor foregroundColor);

    void renderLine(std::string_view text,
                    vtbackend::LineOffset lineOffset,
                    vtbackend::RGBColor foregroundColor,
                    TextStyle style);

    /// Must be invoked when rendering the terminal's text has finished for this frame.
    void endFrame();

  private:
    /// Puts a sequence of codepoints that belong to the same grid cell at @p _pos
    /// at the end of the currently filled line.
    void appendCellTextToClusterGroup(std::u32string_view codepoints,
                                      TextStyle style,
                                      vtbackend::RGBColor color);

    void flushTextClusterGroup();

    void resetAndMovePenForward(vtbackend::ColumnOffset penIncrementInX) noexcept
    {
        _codepoints.clear();
        _clusters.clear();
        _cellCount = 0;
        _initialPenPosition.column += penIncrementInX;
    }

    Events& _events;

    // pen-start position of this text group
    vtbackend::CellLocation _initialPenPosition {};

    // uniform text style for this text group
    TextStyle _style = TextStyle::Invalid;

    // uniform text color for this text group
    vtbackend::RGBColor _color {};

    // codepoints within this text group with
    // uniform unicode properties (script, language, direction).
    std::vector<char32_t> _codepoints;

    // cluster indices for each codepoint
    std::vector<unsigned> _clusters;

    // number of grid cells processed
    int _cellCount = 0; // FIXME: EA width vs actual cells

    bool _forceUpdateInitialPenPosition = false;
};

} // namespace vtrasterizer
