/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <terminal/Color.h>
#include <terminal/ColorPalette.h>
#include <terminal/RenderBufferBuilder.h>

#include <crispy/utils.h>

#include <unicode/convert.h>
#include <unicode/utf8_grapheme_segmenter.h>

using namespace std;

namespace terminal
{

using crispy::beginsWith;

namespace
{
    ColumnCount graphemeClusterWidth(std::u32string_view cluster) noexcept
    {
        assert(!cluster.empty());
        auto baseWidth = ColumnCount::cast_from(unicode::width(cluster[0]));
        for (size_t i = 1; i < cluster.size(); ++i)
            if (auto const codepoint = cluster[i]; codepoint == 0xFE0F)
                return ColumnCount(2);
        return baseWidth;
    }

    constexpr RGBColor makeRGBColor(RGBColorPair actualColors, CellRGBColor configuredColor) noexcept
    {
        if (holds_alternative<CellForegroundColor>(configuredColor))
            return actualColors.foreground;
        if (holds_alternative<CellBackgroundColor>(configuredColor))
            return actualColors.background;
        return get<RGBColor>(configuredColor);
    }

    RGBColorPair makeRGBColorPair(RGBColorPair actualColors,
                                  CellRGBColorAndAlphaPair configuredColor) noexcept
    {
        return RGBColorPair { mix(makeRGBColor(actualColors, configuredColor.foreground),
                                  actualColors.foreground,
                                  configuredColor.foregroundAlpha),
                              mix(makeRGBColor(actualColors, configuredColor.background),
                                  actualColors.background,
                                  configuredColor.backgroundAlpha) }
            .distinct();
    }

    RGBColorPair makeColors(ColorPalette const& _colorPalette,
                            CellFlags _cellFlags,
                            bool _reverseVideo,
                            Color foregroundColor,
                            Color backgroundColor,
                            bool _selected,
                            bool _isCursor,
                            bool _isHighlighted,
                            bool _blink,
                            bool _rapidBlink) noexcept
    {
        auto const sgrColors = makeColors(
            _colorPalette, _cellFlags, _reverseVideo, foregroundColor, backgroundColor, _blink, _rapidBlink);

        if (!_selected && !_isCursor && !_isHighlighted)
            return sgrColors;

        auto getSelectionColor =
            [&](RGBColorPair colorPair, bool selected, ColorPalette const& colors) noexcept -> RGBColorPair {
            if (selected)
                return makeRGBColorPair(sgrColors, colors.selection);
            else
                return colorPair;
        };

        if (!_isCursor && _isHighlighted)
            return makeRGBColorPair(sgrColors, _colorPalette.yankHighlight);

        auto const selectionColors = getSelectionColor(sgrColors, _selected, _colorPalette);
        if (!_isCursor)
            return selectionColors;

        if (!_selected)
            return { makeRGBColor(sgrColors, _colorPalette.cursor.textOverrideColor),
                     makeRGBColor(sgrColors, _colorPalette.cursor.color) };

        Require(_isCursor && _selected);

        auto cursorColor =
            RGBColorPair { makeRGBColor(selectionColors, _colorPalette.cursor.textOverrideColor),
                           makeRGBColor(selectionColors, _colorPalette.cursor.color) };

        return mix(cursorColor, selectionColors, 0.25f).distinct();
    }

} // namespace

template <typename Cell>
RenderBufferBuilder<Cell>::RenderBufferBuilder(Terminal const& _terminal,
                                               RenderBuffer& _output,
                                               LineOffset base,
                                               bool theReverseVideo,
                                               HighlightSearchMatches highlightSearchMatches):
    output { _output },
    terminal { _terminal },
    cursorPosition { _terminal.inputHandler().mode() == ViMode::Insert
                         ? _terminal.realCursorPosition()
                         : _terminal.state().viCommands.cursorPosition },
    baseLine { base },
    reverseVideo { theReverseVideo },
    _highlightSearchMatches { highlightSearchMatches }
{
    output.frameID = _terminal.lastFrameID();
    output.cursor = renderCursor();
}

template <typename Cell>
optional<RenderCursor> RenderBufferBuilder<Cell>::renderCursor() const
{
    if (!terminal.cursorCurrentlyVisible() || !terminal.viewport().isLineVisible(cursorPosition.line))
        return nullopt;

    // TODO: check if CursorStyle has changed, and update render context accordingly.

    auto constexpr InactiveCursorShape = CursorShape::Rectangle; // TODO configurable
    auto const shape = terminal.state().focused ? terminal.cursorShape() : InactiveCursorShape;

    auto const cursorScreenPosition =
        CellLocation { cursorPosition.line + boxed_cast<LineOffset>(terminal.viewport().scrollOffset()),
                       cursorPosition.column };

    auto const cellWidth = terminal.currentScreen().cellWidthAt(cursorPosition);

    return RenderCursor { cursorScreenPosition, shape, cellWidth };
}

template <typename Cell>
RenderCell RenderBufferBuilder<Cell>::makeRenderCellExplicit(ColorPalette const& _colorPalette,
                                                             u32string graphemeCluster,
                                                             ColumnCount width,
                                                             CellFlags flags,
                                                             RGBColor fg,
                                                             RGBColor bg,
                                                             Color ul,
                                                             LineOffset _line,
                                                             ColumnOffset _column)
{
    auto renderCell = RenderCell {};
    renderCell.attributes.backgroundColor = bg;
    renderCell.attributes.foregroundColor = fg;
    renderCell.attributes.decorationColor = getUnderlineColor(_colorPalette, flags, fg, ul);
    renderCell.attributes.flags = flags;
    renderCell.position.line = _line;
    renderCell.position.column = _column;
    renderCell.width = unbox<uint8_t>(width);
    renderCell.codepoints = move(graphemeCluster);
    return renderCell;
}

template <typename Cell>
RenderCell RenderBufferBuilder<Cell>::makeRenderCellExplicit(ColorPalette const& _colorPalette,
                                                             char32_t codepoint,
                                                             CellFlags flags,
                                                             RGBColor fg,
                                                             RGBColor bg,
                                                             Color ul,
                                                             LineOffset _line,
                                                             ColumnOffset _column)
{
    RenderCell renderCell;
    renderCell.attributes.backgroundColor = bg;
    renderCell.attributes.foregroundColor = fg;
    renderCell.attributes.decorationColor = getUnderlineColor(_colorPalette, flags, fg, ul);
    renderCell.attributes.flags = flags;
    renderCell.position.line = _line;
    renderCell.position.column = _column;
    renderCell.width = 1;
    if (codepoint)
        renderCell.codepoints.push_back(codepoint);
    return renderCell;
}

template <typename Cell>
RenderCell RenderBufferBuilder<Cell>::makeRenderCell(ColorPalette const& _colorPalette,
                                                     HyperlinkStorage const& _hyperlinks,
                                                     Cell const& screenCell,
                                                     RGBColor fg,
                                                     RGBColor bg,
                                                     LineOffset _line,
                                                     ColumnOffset _column)
{
    RenderCell renderCell;
    renderCell.attributes.backgroundColor = bg;
    renderCell.attributes.foregroundColor = fg;
    renderCell.attributes.decorationColor = screenCell.getUnderlineColor(_colorPalette, fg);
    renderCell.attributes.flags = screenCell.styles();
    renderCell.position.line = _line;
    renderCell.position.column = _column;
    renderCell.width = screenCell.width();

    if (screenCell.codepointCount() != 0)
    {
        for (size_t i = 0; i < screenCell.codepointCount(); ++i)
            renderCell.codepoints.push_back(screenCell.codepoint(i));
    }

    renderCell.image = screenCell.imageFragment();

    if (auto href = _hyperlinks.hyperlinkById(screenCell.hyperlink()))
    {
        auto const& color = href->state == HyperlinkState::Hover ? _colorPalette.hyperlinkDecoration.hover
                                                                 : _colorPalette.hyperlinkDecoration.normal;
        // TODO(decoration): Move property into Terminal.
        auto const decoration =
            href->state == HyperlinkState::Hover
                ? CellFlags::Underline             // TODO: decorationRenderer_.hyperlinkHover()
                : CellFlags::DottedUnderline;      // TODO: decorationRenderer_.hyperlinkNormal();
        renderCell.attributes.flags |= decoration; // toCellStyle(decoration);
        renderCell.attributes.decorationColor = color;
    }

    return renderCell;
}

template <typename Cell>
RGBColorPair RenderBufferBuilder<Cell>::makeColorsForCell(CellLocation gridPosition,
                                                          CellFlags cellFlags,
                                                          Color foregroundColor,
                                                          Color backgroundColor) const noexcept
{
    auto const hasCursor = gridPosition == cursorPosition;

    // clang-format off
    bool const paintCursor =
        (hasCursor || (prevHasCursor && prevWidth == 2))
            && output.cursor.has_value()
            && output.cursor->shape == CursorShape::Block;
    // clang-format on

    auto const selected = terminal.isSelected(CellLocation { gridPosition.line, gridPosition.column });
    auto const highlighted = terminal.isHighlighted(CellLocation { gridPosition.line, gridPosition.column });
    auto const blink = terminal.blinkState();
    auto const rapidBlink = terminal.rapidBlinkState();

    return makeColors(terminal.colorPalette(),
                      cellFlags,
                      reverseVideo,
                      foregroundColor,
                      backgroundColor,
                      selected,
                      paintCursor,
                      highlighted,
                      blink,
                      rapidBlink);
}

template <typename Cell>
RenderAttributes RenderBufferBuilder<Cell>::createRenderAttributes(
    CellLocation gridPosition, GraphicsAttributes graphicsAttributes) const noexcept
{
    auto const [fg, bg] = makeColorsForCell(gridPosition,
                                            graphicsAttributes.styles,
                                            graphicsAttributes.foregroundColor,
                                            graphicsAttributes.backgroundColor);
    auto renderAttributes = RenderAttributes {};
    renderAttributes.foregroundColor = fg;
    renderAttributes.backgroundColor = bg;
    renderAttributes.decorationColor = getUnderlineColor(
        terminal.colorPalette(), graphicsAttributes.styles, fg, graphicsAttributes.underlineColor);
    renderAttributes.flags = graphicsAttributes.styles;
    return renderAttributes;
}

template <typename Cell>
RenderLine RenderBufferBuilder<Cell>::createRenderLine(TrivialLineBuffer const& lineBuffer,
                                                       LineOffset lineOffset) const
{
    auto const pos = CellLocation { lineOffset, ColumnOffset(0) };
    auto const gridPosition = terminal.viewport().translateScreenToGridCoordinate(pos);
    auto renderLine = RenderLine {};
    renderLine.lineOffset = lineOffset;
    renderLine.usedColumns = lineBuffer.usedColumns;
    renderLine.text = lineBuffer.text.view();
    renderLine.textAttributes = createRenderAttributes(gridPosition, lineBuffer.textAttributes);
    renderLine.fillAttributes = createRenderAttributes(gridPosition, lineBuffer.fillAttributes);

    return renderLine;
}

template <typename Cell>
void RenderBufferBuilder<Cell>::renderTrivialLine(TrivialLineBuffer const& lineBuffer, LineOffset lineOffset)
{
    // if (lineBuffer.text.size())
    //     fmt::print("Rendering trivial line {:2} 0..{}/{} ({} bytes): \"{}\"\n",
    //                lineOffset.value,
    //                lineBuffer.usedColumns,
    //                lineBuffer.displayWidth,
    //                lineBuffer.text.size(),
    //                lineBuffer.text.view());

    auto const frontIndex = output.cells.size();

    // TODO: visual selection can alter colors for some columns in this line.
    // In that case, it seems like we cannot just pass it bare over but have to take the slower path.
    // But that should be fine.
    bool const canRenderViaSimpleLine = false; // <- Should be false if selection covers this line.

    if (canRenderViaSimpleLine)
    {
        output.lines.emplace_back(createRenderLine(lineBuffer, lineOffset));
        lineNr = lineOffset;
        prevWidth = 0;
        prevHasCursor = false;
        return;
    }

    auto const textMargin = min(boxed_cast<ColumnOffset>(terminal.pageSize().columns),
                                ColumnOffset::cast_from(lineBuffer.usedColumns));
    auto const pageColumnsEnd = boxed_cast<ColumnOffset>(terminal.pageSize().columns);

    // {{{ render text
    auto graphemeClusterSegmenter = unicode::utf8_grapheme_segmenter(lineBuffer.text.view());
    auto columnOffset = ColumnOffset(0);

    searchPatternOffset = 0;

    for (u32string const& graphemeCluster: graphemeClusterSegmenter)
    {
        auto const pos = CellLocation { lineOffset, columnOffset };
        auto const gridPosition = terminal.viewport().translateScreenToGridCoordinate(pos);
        auto const [fg, bg] = makeColorsForCell(gridPosition,
                                                lineBuffer.textAttributes.styles,
                                                lineBuffer.textAttributes.foregroundColor,
                                                lineBuffer.textAttributes.backgroundColor);
        auto const width = graphemeClusterWidth(graphemeCluster);
        // fmt::print(" start {}, count {}, bytes {}, grapheme cluster \"{}\"\n",
        //            columnOffset,
        //            width,
        //            unicode::convert_to<char>(u32string_view(graphemeCluster)).size(),
        //            unicode::convert_to<char>(u32string_view(graphemeCluster)));

        output.cells.emplace_back(makeRenderCellExplicit(terminal.colorPalette(),
                                                         graphemeCluster,
                                                         width,
                                                         lineBuffer.textAttributes.styles,
                                                         fg,
                                                         bg,
                                                         lineBuffer.textAttributes.underlineColor,
                                                         baseLine + lineOffset,
                                                         columnOffset));

        columnOffset += ColumnOffset::cast_from(width);
        lineNr = lineOffset;
        prevWidth = 0;
        prevHasCursor = false;

        matchSearchPattern(u32string_view(graphemeCluster));
    }
    // }}}

    // {{{ fill the remaining empty cells
    for (auto columnOffset = textMargin; columnOffset < pageColumnsEnd; ++columnOffset)
    {
        auto const pos = CellLocation { lineOffset, columnOffset };
        auto const gridPosition = terminal.viewport().translateScreenToGridCoordinate(pos);
        auto renderAttributes = createRenderAttributes(gridPosition, lineBuffer.fillAttributes);

        output.cells.emplace_back(makeRenderCellExplicit(terminal.colorPalette(),
                                                         char32_t { 0 },
                                                         lineBuffer.fillAttributes.styles,
                                                         renderAttributes.foregroundColor,
                                                         renderAttributes.backgroundColor,
                                                         lineBuffer.fillAttributes.underlineColor,
                                                         baseLine + lineOffset,
                                                         columnOffset));
    }
    // }}}

    auto const backIndex = output.cells.size() - 1;

    output.cells[frontIndex].groupStart = true;
    output.cells[backIndex].groupEnd = true;
}

template <typename Cell>
template <typename T>
void RenderBufferBuilder<Cell>::matchSearchPattern(T const& textCell)
{
    if (_highlightSearchMatches == HighlightSearchMatches::No)
        return;

    auto const& searchMode = terminal.state().searchMode;
    if (searchMode.pattern.empty())
        return;

    if (!beginsWith(u32string_view(searchMode.pattern.data() + searchPatternOffset,
                                   searchMode.pattern.size() - searchPatternOffset),
                    textCell))
    {
        // match fail
        searchPatternOffset = 0;
        return;
    }

    if constexpr (std::is_same_v<Cell, T>)
        searchPatternOffset += textCell.codepointCount();
    else
        searchPatternOffset += textCell.size();

    if (searchPatternOffset < searchMode.pattern.size())
        return; // match incomplete

    // match complete

    auto const offsetIntoFront = output.cells.size() - searchPatternOffset;

    auto const isFocusedMatch =
        CellLocationRange {
            output.cells[offsetIntoFront].position,
            output.cells.back().position,
        }
            .contains(terminal.viewport().translateGridToScreenCoordinate(
                terminal.state().viCommands.cursorPosition));

    for (size_t i = offsetIntoFront; i < output.cells.size(); ++i)
    {
        auto& cellAttributes = output.cells[i].attributes;

        auto const searchMatchColors =
            makeRGBColorPair(RGBColorPair { cellAttributes.foregroundColor, cellAttributes.backgroundColor },
                             isFocusedMatch ? terminal.colorPalette().searchHighlightFocused
                                            : terminal.colorPalette().searchHighlight);

        cellAttributes.backgroundColor = searchMatchColors.background;
        cellAttributes.foregroundColor = searchMatchColors.foreground;
    }
    searchPatternOffset = 0;
}

template <typename Cell>
void RenderBufferBuilder<Cell>::startLine(LineOffset _line) noexcept
{
    isNewLine = true;
    lineNr = _line;
    prevWidth = 0;
    prevHasCursor = false;
}

template <typename Cell>
void RenderBufferBuilder<Cell>::endLine() noexcept
{
    if (!output.cells.empty())
    {
        output.cells.back().groupEnd = true;
    }
}

template <typename Cell>
void RenderBufferBuilder<Cell>::renderCell(Cell const& screenCell, LineOffset _line, ColumnOffset _column)
{
    auto const pos = CellLocation { _line, _column };
    auto const gridPosition = terminal.viewport().translateScreenToGridCoordinate(pos);
    auto /*const*/ [fg, bg] = makeColorsForCell(
        gridPosition, screenCell.styles(), screenCell.foregroundColor(), screenCell.backgroundColor());

    prevWidth = screenCell.width();
    prevHasCursor = gridPosition == cursorPosition;

    auto const cellEmpty = screenCell.empty();
    auto const customBackground = bg != terminal.colorPalette().defaultBackground || !!screenCell.styles();

    switch (state)
    {
        case State::Gap:
            if (!cellEmpty || customBackground)
            {
                state = State::Sequence;
                output.cells.emplace_back(makeRenderCell(terminal.colorPalette(),
                                                         terminal.state().hyperlinks,
                                                         screenCell,
                                                         fg,
                                                         bg,
                                                         baseLine + _line,
                                                         _column));
                output.cells.back().groupStart = true;
            }
            break;
        case State::Sequence:
            if (cellEmpty && !customBackground)
            {
                output.cells.back().groupEnd = true;
                state = State::Gap;
            }
            else
            {
                output.cells.emplace_back(makeRenderCell(terminal.colorPalette(),
                                                         terminal.state().hyperlinks,
                                                         screenCell,
                                                         fg,
                                                         bg,
                                                         baseLine + _line,
                                                         _column));

                if (isNewLine)
                    output.cells.back().groupStart = true;
            }
            break;
    }
    isNewLine = false;

    matchSearchPattern(screenCell);
}

} // namespace terminal

#include <terminal/Cell.h>
template class terminal::RenderBufferBuilder<terminal::Cell>;
