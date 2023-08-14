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

#include <vtbackend/CellUtil.h>
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/RenderBufferBuilder.h>

#include <crispy/utils.h>

#include "vtbackend/RenderBuffer.h"
#include "vtbackend/primitives.h"
#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>

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

    constexpr rgb_color makeRGBColor(rgb_color_pair actualColors, cell_rgb_color configuredColor) noexcept
    {
        if (holds_alternative<cell_foreground_color>(configuredColor))
            return actualColors.foreground;
        if (holds_alternative<cell_background_color>(configuredColor))
            return actualColors.background;
        return get<rgb_color>(configuredColor);
    }

    rgb_color_pair makeRGBColorPair(rgb_color_pair actualColors,
                                    cell_rgb_color_and_alpha_pair configuredColor) noexcept
    {
        return rgb_color_pair { mix(makeRGBColor(actualColors, configuredColor.foreground),
                                    actualColors.foreground,
                                    configuredColor.foregroundAlpha),
                                mix(makeRGBColor(actualColors, configuredColor.background),
                                    actualColors.background,
                                    configuredColor.backgroundAlpha) }
            .distinct();
    }

    rgb_color_pair makeColors(color_palette const& colorPalette,
                              cell_flags cellFlags,
                              bool reverseVideo,
                              color foregroundColor,
                              color backgroundColor,
                              bool selected,
                              bool isCursor,
                              bool isCursorLine,
                              bool isHighlighted,
                              bool blink,
                              bool rapidBlink) noexcept
    {
        auto sgrColors = CellUtil::makeColors(
            colorPalette, cellFlags, reverseVideo, foregroundColor, backgroundColor, blink, rapidBlink);

        if (isCursorLine)
            sgrColors = makeRGBColorPair(sgrColors, colorPalette.normalModeCursorline);

        if (!selected && !isCursor && !isHighlighted)
            return sgrColors;

        auto getSelectionColor = [&](rgb_color_pair colorPair,
                                     bool selected,
                                     color_palette const& colors) noexcept -> rgb_color_pair {
            if (selected)
                return makeRGBColorPair(sgrColors, colors.selection);
            else
                return colorPair;
        };

        if (!isCursor && isHighlighted)
            return makeRGBColorPair(sgrColors, colorPalette.yankHighlight);

        auto const selectionColors = getSelectionColor(sgrColors, selected, colorPalette);
        if (!isCursor)
            return selectionColors;

        if (!selected)
            return rgb_color_pair { makeRGBColor(sgrColors, colorPalette.cursor.textOverrideColor),
                                    makeRGBColor(sgrColors, colorPalette.cursor.color) }
                .distinct();

        Require(isCursor && selected);

        auto cursorColor =
            rgb_color_pair { makeRGBColor(selectionColors, colorPalette.cursor.textOverrideColor),
                             makeRGBColor(selectionColors, colorPalette.cursor.color) };

        return mix(cursorColor, selectionColors, 0.25f).distinct();
    }

} // namespace

template <typename Cell>
render_buffer_builder<Cell>::render_buffer_builder(Terminal const& terminal,
                                                   render_buffer& output,
                                                   line_offset base,
                                                   bool theReverseVideo,
                                                   highlight_search_matches highlightSearchMatches,
                                                   input_method_data inputMethodData,
                                                   optional<cell_location> theCursorPosition,
                                                   bool includeSelection):
    _output { output },
    _terminal { terminal },
    _cursorPosition { theCursorPosition },
    _baseLine { base },
    _reverseVideo { theReverseVideo },
    _highlightSearchMatches { highlightSearchMatches },
    _inputMethodData { std::move(inputMethodData) },
    _includeSelection { includeSelection }
{
    output.frameID = terminal.lastFrameID();

    if (_cursorPosition)
        output.cursor = renderCursor();
}

template <typename Cell>
optional<render_cursor> render_buffer_builder<Cell>::renderCursor() const
{
    if (!_cursorPosition || !_terminal.cursorCurrentlyVisible()
        || !_terminal.get_viewport().isLineVisible(_cursorPosition->line))
        return nullopt;

    // TODO: check if CursorStyle has changed, and update render context accordingly.

    auto constexpr InactiveCursorShape = cursor_shape::Rectangle; // TODO configurable
    auto const shape = _terminal.state().focused ? _terminal.cursorShape() : InactiveCursorShape;

    auto const cursorScreenPosition =
        cell_location { _baseLine + _cursorPosition->line
                            + boxed_cast<line_offset>(_terminal.get_viewport().scrollOffset()),
                        _cursorPosition->column };

    auto const cellWidth = _terminal.currentScreen().cellWidthAt(*_cursorPosition);

    return render_cursor { cursorScreenPosition, shape, cellWidth };
}

template <typename Cell>
render_cell render_buffer_builder<Cell>::makeRenderCellExplicit(color_palette const& colorPalette,
                                                                u32string graphemeCluster,
                                                                ColumnCount width,
                                                                cell_flags flags,
                                                                rgb_color fg,
                                                                rgb_color bg,
                                                                color ul,
                                                                line_offset line,
                                                                column_offset column)
{
    auto renderCell = render_cell {};
    renderCell.attributes.backgroundColor = bg;
    renderCell.attributes.foregroundColor = fg;
    renderCell.attributes.decorationColor = CellUtil::makeUnderlineColor(colorPalette, fg, ul, flags);
    renderCell.attributes.flags = flags;
    renderCell.position.line = line;
    renderCell.position.column = column;
    renderCell.width = unbox<uint8_t>(width);
    renderCell.codepoints = std::move(graphemeCluster);
    return renderCell;
}

template <typename Cell>
render_cell render_buffer_builder<Cell>::makeRenderCellExplicit(color_palette const& colorPalette,
                                                                char32_t codepoint,
                                                                cell_flags flags,
                                                                rgb_color fg,
                                                                rgb_color bg,
                                                                color ul,
                                                                line_offset line,
                                                                column_offset column)
{
    render_cell renderCell;
    renderCell.attributes.backgroundColor = bg;
    renderCell.attributes.foregroundColor = fg;
    renderCell.attributes.decorationColor = CellUtil::makeUnderlineColor(colorPalette, fg, ul, flags);
    renderCell.attributes.flags = flags;
    renderCell.position.line = line;
    renderCell.position.column = column;
    renderCell.width = 1;
    if (codepoint)
        renderCell.codepoints.push_back(codepoint);
    return renderCell;
}

template <typename Cell>
render_cell render_buffer_builder<Cell>::makeRenderCell(color_palette const& colorPalette,
                                                        hyperlink_storage const& hyperlinks,
                                                        Cell const& screenCell,
                                                        rgb_color fg,
                                                        rgb_color bg,
                                                        line_offset line,
                                                        column_offset column)
{
    render_cell renderCell;
    renderCell.attributes.backgroundColor = bg;
    renderCell.attributes.foregroundColor = fg;
    renderCell.attributes.decorationColor = CellUtil::makeUnderlineColor(colorPalette, fg, screenCell);
    renderCell.attributes.flags = screenCell.flags();
    renderCell.position.line = line;
    renderCell.position.column = column;
    renderCell.width = screenCell.width();

    if (screenCell.codepointCount() != 0)
    {
        for (size_t i = 0; i < screenCell.codepointCount(); ++i)
            renderCell.codepoints.push_back(screenCell.codepoint(i));
    }

    renderCell.image = screenCell.imageFragment();

    if (auto href = hyperlinks.hyperlinkById(screenCell.hyperlink()))
    {
        auto const& color = href->state == hyperlink_state::Hover ? colorPalette.hyperlinkDecoration.hover
                                                                  : colorPalette.hyperlinkDecoration.normal;
        // TODO(decoration): Move property into Terminal.
        auto const decoration =
            href->state == hyperlink_state::Hover
                ? cell_flags::Underline            // TODO: decorationRenderer_.hyperlinkHover()
                : cell_flags::DottedUnderline;     // TODO: decorationRenderer_.hyperlinkNormal();
        renderCell.attributes.flags |= decoration; // toCellStyle(decoration);
        renderCell.attributes.decorationColor = color;
    }

    return renderCell;
}

template <typename Cell>
rgb_color_pair render_buffer_builder<Cell>::makeColorsForCell(cell_location gridPosition,
                                                              cell_flags cellFlags,
                                                              color foregroundColor,
                                                              color backgroundColor) const noexcept
{
    auto const hasCursor = _cursorPosition && gridPosition == *_cursorPosition;

    // clang-format off
    bool const paintCursor =
        (hasCursor || (_prevHasCursor && _prevWidth == 2))
            && _output.cursor.has_value()
            && _output.cursor->shape == cursor_shape::Block;
    // clang-format on

    auto const selected =
        _includeSelection && _terminal.isSelected(cell_location { gridPosition.line, gridPosition.column });
    auto const highlighted =
        _terminal.isHighlighted(cell_location { gridPosition.line, gridPosition.column });
    auto const blink = _terminal.blinkState();
    auto const rapidBlink = _terminal.rapidBlinkState();

    return makeColors(_terminal.colorPalette(),
                      cellFlags,
                      _reverseVideo,
                      foregroundColor,
                      backgroundColor,
                      selected,
                      paintCursor,
                      _useCursorlineColoring,
                      highlighted,
                      blink,
                      rapidBlink);
}

template <typename Cell>
render_attributes render_buffer_builder<Cell>::createRenderAttributes(
    cell_location gridPosition, graphics_attributes graphicsAttributes) const noexcept
{
    auto const [fg, bg] = makeColorsForCell(gridPosition,
                                            graphicsAttributes.flags,
                                            graphicsAttributes.foregroundColor,
                                            graphicsAttributes.backgroundColor);
    auto renderAttributes = render_attributes {};
    renderAttributes.foregroundColor = fg;
    renderAttributes.backgroundColor = bg;
    renderAttributes.decorationColor = CellUtil::makeUnderlineColor(
        _terminal.colorPalette(), fg, graphicsAttributes.underlineColor, graphicsAttributes.flags);
    renderAttributes.flags = graphicsAttributes.flags;
    return renderAttributes;
}

template <typename Cell>
render_line render_buffer_builder<Cell>::createRenderLine(trivial_line_buffer const& lineBuffer,
                                                          line_offset lineOffset) const
{
    auto const pos = cell_location { lineOffset, column_offset(0) };
    auto const gridPosition = _terminal.get_viewport().translateScreenToGridCoordinate(pos);
    auto renderLine = render_line {};
    renderLine.lineOffset = lineOffset;
    renderLine.usedColumns = lineBuffer.usedColumns;
    renderLine.displayWidth = _terminal.pageSize().columns;
    renderLine.text = lineBuffer.text.view();
    renderLine.textAttributes = createRenderAttributes(gridPosition, lineBuffer.textAttributes);
    renderLine.fillAttributes = createRenderAttributes(gridPosition, lineBuffer.fillAttributes);

    return renderLine;
}

template <typename Cell>
bool render_buffer_builder<Cell>::gridLineContainsCursor(line_offset lineOffset) const noexcept
{
    if (_terminal.currentScreen().getCursor().position.line == lineOffset)
        return true;

    if (_cursorPosition && _terminal.state().inputHandler.mode() != vi_mode::Insert)
    {
        auto const viCursor = _terminal.get_viewport().translateGridToScreenCoordinate(_cursorPosition->line);
        if (viCursor == lineOffset)
            return true;
    }

    return false;
}

template <typename Cell>
void render_buffer_builder<Cell>::renderTrivialLine(trivial_line_buffer const& lineBuffer,
                                                    line_offset lineOffset)
{
    // if (lineBuffer.text.size())
    //     fmt::print("Rendering trivial line {:2} 0..{}/{} ({} bytes): \"{}\"\n",
    //                lineOffset.value,
    //                lineBuffer.usedColumns,
    //                lineBuffer.displayWidth,
    //                lineBuffer.text.size(),
    //                lineBuffer.text.view());

    // No need to call isCursorLine(lineOffset) because lines containing a cursor are always inflated.
    _useCursorlineColoring = false;

    auto const frontIndex = _output.cells.size();

    // Visual selection can alter colors for some columns in this line.
    // In that case, it seems like we cannot just pass it bare over but have to take the slower path.
    // But that should be fine.
    //
    // Testing for the cursor's current line is made because the cursor might be a block cursor,
    // which affects background/foreground color again.
    // We're not testing for cursor shape (which should be done in order to be 100% correct)
    // because it's not really draining performance.
    bool const canRenderViaSimpleLine =
        (!_terminal.isSelected(lineOffset) || !_includeSelection) && !gridLineContainsCursor(lineOffset);

    if (canRenderViaSimpleLine)
    {
        _output.lines.emplace_back(createRenderLine(lineBuffer, lineOffset));
        _lineNr = lineOffset;
        _prevWidth = 0;
        _prevHasCursor = false;
        return;
    }

    auto const textMargin = min(boxed_cast<column_offset>(_terminal.pageSize().columns),
                                column_offset::cast_from(lineBuffer.usedColumns));
    auto const pageColumnsEnd = boxed_cast<column_offset>(_terminal.pageSize().columns);

    // render text
    _searchPatternOffset = 0;
    renderUtf8Text(cell_location { lineOffset, column_offset(0) },
                   lineBuffer.textAttributes,
                   lineBuffer.text.view(),
                   true);

    // {{{ fill the remaining empty cells
    for (auto columnOffset = textMargin; columnOffset < pageColumnsEnd; ++columnOffset)
    {
        auto const pos = cell_location { lineOffset, columnOffset };
        auto const gridPosition = _terminal.get_viewport().translateScreenToGridCoordinate(pos);
        auto renderAttributes = createRenderAttributes(gridPosition, lineBuffer.fillAttributes);

        _output.cells.emplace_back(makeRenderCellExplicit(_terminal.colorPalette(),
                                                          char32_t { 0 },
                                                          lineBuffer.fillAttributes.flags,
                                                          renderAttributes.foregroundColor,
                                                          renderAttributes.backgroundColor,
                                                          lineBuffer.fillAttributes.underlineColor,
                                                          _baseLine + lineOffset,
                                                          columnOffset));
    }
    // }}}

    auto const backIndex = _output.cells.size() - 1;

    _output.cells[frontIndex].groupStart = true;
    _output.cells[backIndex].groupEnd = true;
}

template <typename Cell>
template <typename T>
void render_buffer_builder<Cell>::matchSearchPattern(T const& cellText)
{
    if (_highlightSearchMatches == highlight_search_matches::No)
        return;

    auto const& searchMode = _terminal.state().searchMode;
    if (searchMode.pattern.empty())
        return;

    auto const isFullMatch = [&]() -> bool {
        if constexpr (std::is_same_v<Cell, T>)
        {
            return !CellUtil::beginsWith(u32string_view(searchMode.pattern.data() + _searchPatternOffset,
                                                        searchMode.pattern.size() - _searchPatternOffset),
                                         cellText);
        }
        else
        {
            return crispy::beginsWith(u32string_view(searchMode.pattern.data() + _searchPatternOffset,
                                                     searchMode.pattern.size() - _searchPatternOffset),
                                      cellText);
        }
    }();

    if (isFullMatch)
    {
        // match fail
        _searchPatternOffset = 0;
        return;
    }

    if constexpr (std::is_same_v<Cell, T>)
        _searchPatternOffset += cellText.codepointCount();
    else
        _searchPatternOffset += cellText.size();

    if (_searchPatternOffset < searchMode.pattern.size())
        return; // match incomplete

    // match complete

    auto const offsetIntoFront = _output.cells.size() - _searchPatternOffset;

    auto const isFocusedMatch =
        cell_location_range {
            _output.cells[offsetIntoFront].position,
            _output.cells.back().position,
        }
            .contains(_terminal.get_viewport().translateGridToScreenCoordinate(
                _terminal.state().viCommands.cursorPosition));

    auto highlightColors = [&]() -> cell_rgb_color_and_alpha_pair {
        // Oh yeah, this can be optimized :)
        if (isFocusedMatch)
        {
            if (_terminal.state().searchMode.initiatedByDoubleClick)
                return _terminal.colorPalette().wordHighlightCurrent;
            else
                return _terminal.colorPalette().searchHighlightFocused;
        }
        else
        {
            if (_terminal.state().searchMode.initiatedByDoubleClick)
                return _terminal.colorPalette().wordHighlight;
            else
                return _terminal.colorPalette().searchHighlight;
        }
    }();

    for (size_t i = offsetIntoFront; i < _output.cells.size(); ++i)
    {
        auto& cellAttributes = _output.cells[i].attributes;
        auto const actualColors =
            rgb_color_pair { cellAttributes.foregroundColor, cellAttributes.backgroundColor };
        auto const searchMatchColors = makeRGBColorPair(actualColors, highlightColors);

        cellAttributes.backgroundColor = searchMatchColors.background;
        cellAttributes.foregroundColor = searchMatchColors.foreground;
    }
    _searchPatternOffset = 0;
}

template <typename Cell>
void render_buffer_builder<Cell>::startLine(line_offset line) noexcept
{
    _lineNr = line;
    _prevWidth = 0;
    _prevHasCursor = false;

    _useCursorlineColoring = isCursorLine(line);
}

template <typename Cell>
bool render_buffer_builder<Cell>::isCursorLine(line_offset line) const noexcept
{
    return _terminal.inputHandler().mode() != vi_mode::Insert && _cursorPosition
           && line
                  == _terminal.get_viewport()
                         .translateGridToScreenCoordinate(cell_location { _cursorPosition->line, {} })
                         .line;
}

template <typename Cell>
void render_buffer_builder<Cell>::endLine() noexcept
{
    if (!_output.cells.empty())
    {
        _output.cells.back().groupEnd = true;
    }
}

template <typename Cell>
ColumnCount render_buffer_builder<Cell>::renderUtf8Text(cell_location screenPosition,
                                                        graphics_attributes textAttributes,
                                                        std::string_view text,
                                                        bool allowMatchSearchPattern)
{
    auto columnCountRendered = ColumnCount(0);

    auto graphemeClusterSegmenter = unicode::utf8_grapheme_segmenter(text);
    for (u32string const& graphemeCluster: graphemeClusterSegmenter)
    {
        auto const gridPosition = _terminal.get_viewport().translateScreenToGridCoordinate(
            screenPosition + column_offset::cast_from(columnCountRendered));
        auto const [fg, bg] = makeColorsForCell(gridPosition,
                                                textAttributes.flags,
                                                textAttributes.foregroundColor,
                                                textAttributes.backgroundColor);
        auto const width = graphemeClusterWidth(graphemeCluster);
        // fmt::print(" start {}, count {}, bytes {}, grapheme cluster \"{}\"\n",
        //            columnOffset,
        //            width,
        //            unicode::convert_to<char>(u32string_view(graphemeCluster)).size(),
        //            unicode::convert_to<char>(u32string_view(graphemeCluster)));

        _output.cells.emplace_back(
            makeRenderCellExplicit(_terminal.colorPalette(),
                                   graphemeCluster,
                                   width,
                                   textAttributes.flags,
                                   fg,
                                   bg,
                                   textAttributes.underlineColor,
                                   _baseLine + screenPosition.line,
                                   screenPosition.column + column_offset::cast_from(columnCountRendered)));

        // Span filling cells for preciding wide glyphs to get the background color properly painted.
        for (auto i = ColumnCount(1); i < width; ++i)
        {
            _output.cells.emplace_back(makeRenderCellExplicit(
                _terminal.colorPalette(),
                U" ", // {}
                ColumnCount(1),
                textAttributes.flags,
                fg,
                bg,
                textAttributes.underlineColor,
                _baseLine + screenPosition.line,
                screenPosition.column + column_offset::cast_from(columnCountRendered + i)));
        }

        columnCountRendered += ColumnCount::cast_from(width);
        _lineNr = screenPosition.line;
        _prevWidth = 0;
        _prevHasCursor = false;

        if (allowMatchSearchPattern)
            matchSearchPattern(u32string_view(graphemeCluster));
    }
    return columnCountRendered;
}

template <typename Cell>
bool render_buffer_builder<Cell>::tryRenderInputMethodEditor(cell_location screenPosition,
                                                             cell_location gridPosition)
{
    // Render IME preeditString if available and screen position matches cursor position.
    if (_cursorPosition && gridPosition == *_cursorPosition && !_inputMethodData.preeditString.empty())
    {
        auto const inputMethodEditorStyles = _terminal.colorPalette().inputMethodEditor;
        auto textAttributes = graphics_attributes {};
        textAttributes.foregroundColor = inputMethodEditorStyles.foreground;
        textAttributes.backgroundColor = inputMethodEditorStyles.background;
        textAttributes.flags |= cell_flags::Bold | cell_flags::Underline;

        if (!_output.cells.empty())
            _output.cells.back().groupEnd = true;

        _inputMethodSkipColumns =
            renderUtf8Text(screenPosition, textAttributes, _inputMethodData.preeditString, false);
        if (_inputMethodSkipColumns > ColumnCount(0))
        {
            _output.cursor->position.column += column_offset::cast_from(_inputMethodSkipColumns);
            _output.cells.at(_output.cells.size() - unbox<size_t>(_inputMethodSkipColumns)).groupStart = true;
            _output.cells.back().groupEnd = true;
        }
    }

    if (_inputMethodSkipColumns == ColumnCount(0))
        return false;

    // Skipping grid cells that have already been rendered due to IME.
    _inputMethodSkipColumns--;
    return true;
}

template <typename Cell>
void render_buffer_builder<Cell>::renderCell(Cell const& screenCell, line_offset line, column_offset column)
{
    auto const screenPosition = cell_location { line, column };
    auto const gridPosition = _terminal.get_viewport().translateScreenToGridCoordinate(screenPosition);

    if (tryRenderInputMethodEditor(screenPosition, gridPosition))
        return;

    auto /*const*/ [fg, bg] = makeColorsForCell(
        gridPosition, screenCell.flags(), screenCell.foregroundColor(), screenCell.backgroundColor());

    _prevWidth = screenCell.width();
    _prevHasCursor = _cursorPosition && gridPosition == *_cursorPosition;

    _output.cells.emplace_back(makeRenderCell(_terminal.colorPalette(),
                                              _terminal.state().hyperlinks,
                                              screenCell,
                                              fg,
                                              bg,
                                              _baseLine + line,
                                              column));

    if (column == column_offset(0))
        _output.cells.back().groupStart = true;

    matchSearchPattern(screenCell);
}

} // namespace terminal

#include <vtbackend/cell/CompactCell.h>
template class terminal::render_buffer_builder<terminal::compact_cell>;

#include <vtbackend/cell/SimpleCell.h>
template class terminal::render_buffer_builder<terminal::simple_cell>;
