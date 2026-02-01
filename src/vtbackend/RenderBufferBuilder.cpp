// SPDX-License-Identifier: Apache-2.0

#include <vtbackend/CellUtil.h>
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/RenderBufferBuilder.h>

#include <crispy/utils.h>

#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>

using namespace std;

namespace vtbackend
{

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
        return RGBColorPair { .foreground = mix(makeRGBColor(actualColors, configuredColor.foreground),
                                                actualColors.foreground,
                                                configuredColor.foregroundAlpha),
                              .background = mix(makeRGBColor(actualColors, configuredColor.background),
                                                actualColors.background,
                                                configuredColor.backgroundAlpha) }
            .distinct();
    }

    RGBColorPair makeColors(ColorPalette const& colorPalette,
                            CellFlags cellFlags,
                            bool reverseVideo,
                            Color foregroundColor,
                            Color backgroundColor,
                            bool selected,
                            bool isCursor,
                            bool isCursorLine,
                            bool isHighlighted,
                            float blink,
                            float rapidBlink) noexcept
    {
        auto sgrColors = CellUtil::makeColors(
            colorPalette, cellFlags, reverseVideo, foregroundColor, backgroundColor, blink, rapidBlink);

        if (isCursorLine)
            sgrColors = makeRGBColorPair(sgrColors, colorPalette.normalModeCursorline);

        if (!selected && !isCursor && !isHighlighted)
            return sgrColors;

        auto getSelectionColor =
            [&](RGBColorPair colorPair, bool selected, ColorPalette const& colors) noexcept -> RGBColorPair {
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
            return RGBColorPair { .foreground =
                                      makeRGBColor(sgrColors, colorPalette.cursor.textOverrideColor),
                                  .background = makeRGBColor(sgrColors, colorPalette.cursor.color) }
                .distinct();

        Require(isCursor && selected);

        auto cursorColor =
            RGBColorPair { .foreground = makeRGBColor(selectionColors, colorPalette.cursor.textOverrideColor),
                           .background = makeRGBColor(selectionColors, colorPalette.cursor.color) };

        return mix(cursorColor, selectionColors, 0.25f).distinct();
    }

} // namespace

template <CellConcept Cell>
RenderBufferBuilder<Cell>::RenderBufferBuilder(Terminal const& terminal,
                                               RenderBuffer& output,
                                               LineOffset base,
                                               bool theReverseVideo,
                                               HighlightSearchMatches highlightSearchMatches,
                                               InputMethodData inputMethodData,
                                               optional<CellLocation> theCursorPosition,
                                               bool includeSelection):
    _output { &output },
    _terminal { &terminal },
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

template <CellConcept Cell>
optional<RenderCursor> RenderBufferBuilder<Cell>::renderCursor() const
{
    if (!_cursorPosition || !_terminal->cursorCurrentlyVisible()
        || !_terminal->viewport().isLineVisible(_cursorPosition->line))
        return nullopt;

    // TODO: check if CursorStyle has changed, and update render context accordingly.

    auto constexpr InactiveCursorShape = CursorShape::Rectangle; // TODO configurable
    auto const shape = _terminal->focused() ? _terminal->cursorShape() : InactiveCursorShape;

    auto const cursorScreenPosition =
        CellLocation { .line = _baseLine + _cursorPosition->line
                               + boxed_cast<LineOffset>(_terminal->viewport().scrollOffset()),
                       .column = _cursorPosition->column };

    auto const cellWidth = _terminal->currentScreen().cellWidthAt(*_cursorPosition);

    return RenderCursor { .position = cursorScreenPosition, .shape = shape, .width = cellWidth };
}

template <CellConcept Cell>
RenderCell RenderBufferBuilder<Cell>::makeRenderCellExplicit(ColorPalette const& colorPalette,
                                                             u32string graphemeCluster,
                                                             ColumnCount width,
                                                             CellFlags flags,
                                                             LineFlags lineFlags,
                                                             RGBColor fg,
                                                             RGBColor bg,
                                                             Color ul,
                                                             LineOffset line,
                                                             ColumnOffset column)
{
    auto renderCell = RenderCell {};
    renderCell.attributes.backgroundColor = bg;
    renderCell.attributes.foregroundColor = fg;
    renderCell.attributes.decorationColor = CellUtil::makeUnderlineColor(colorPalette, fg, ul, flags);
    renderCell.attributes.flags = flags;
    renderCell.attributes.lineFlags = lineFlags;
    renderCell.position.line = line;
    renderCell.position.column = column;
    renderCell.width = unbox<uint8_t>(width);
    renderCell.codepoints = std::move(graphemeCluster);
    return renderCell;
}

template <CellConcept Cell>
RenderCell RenderBufferBuilder<Cell>::makeRenderCellExplicit(ColorPalette const& colorPalette,
                                                             char32_t codepoint,
                                                             CellFlags flags,
                                                             LineFlags lineFlags,
                                                             RGBColor fg,
                                                             RGBColor bg,
                                                             Color ul,
                                                             LineOffset line,
                                                             ColumnOffset column)
{
    RenderCell renderCell;
    renderCell.attributes.backgroundColor = bg;
    renderCell.attributes.foregroundColor = fg;
    renderCell.attributes.decorationColor = CellUtil::makeUnderlineColor(colorPalette, fg, ul, flags);
    renderCell.attributes.flags = flags;
    renderCell.attributes.lineFlags = lineFlags;
    renderCell.position.line = line;
    renderCell.position.column = column;
    renderCell.width = 1;
    if (codepoint)
        renderCell.codepoints.push_back(codepoint);
    return renderCell;
}

template <CellConcept Cell>
RenderCell RenderBufferBuilder<Cell>::makeRenderCell(ColorPalette const& colorPalette,
                                                     HyperlinkStorage const& hyperlinks,
                                                     Cell const& screenCell,
                                                     LineFlags lineFlags,
                                                     RGBColor fg,
                                                     RGBColor bg,
                                                     LineOffset line,
                                                     ColumnOffset column)
{
    RenderCell renderCell;
    renderCell.attributes.backgroundColor = bg;
    renderCell.attributes.foregroundColor = fg;
    renderCell.attributes.decorationColor = CellUtil::makeUnderlineColor(colorPalette, fg, screenCell);
    renderCell.attributes.flags = screenCell.flags();
    renderCell.attributes.lineFlags = lineFlags;
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
        auto const& color = href->state == HyperlinkState::Hover ? colorPalette.hyperlinkDecoration.hover
                                                                 : colorPalette.hyperlinkDecoration.normal;
        // TODO(decoration): Move property into Terminal.
        auto const decoration =
            href->state == HyperlinkState::Hover
                ? CellFlag::Underline              // TODO: decorationRenderer_.hyperlinkHover()
                : CellFlag::DottedUnderline;       // TODO: decorationRenderer_.hyperlinkNormal();
        renderCell.attributes.flags |= decoration; // toCellStyle(decoration);
        renderCell.attributes.decorationColor = color;
    }

    return renderCell;
}

template <CellConcept Cell>
RGBColorPair RenderBufferBuilder<Cell>::makeColorsForCell(CellLocation gridPosition,
                                                          CellFlags cellFlags,
                                                          Color foregroundColor,
                                                          Color backgroundColor) const noexcept
{
    auto const hasCursor = _cursorPosition && gridPosition == *_cursorPosition;

    // clang-format off
    bool const paintCursor =
        (hasCursor || (_prevHasCursor && _prevWidth == 2))
            && _output->cursor.has_value()
            && _output->cursor->shape == CursorShape::Block
            && _output->cursor->animationProgress >= 1.0f;  // Don't invert cell during animation
    // clang-format on

    auto const selected =
        _includeSelection
        && _terminal->isSelected(CellLocation { .line = gridPosition.line, .column = gridPosition.column });
    auto const highlighted =
        _terminal->isHighlighted(CellLocation { .line = gridPosition.line, .column = gridPosition.column });
    auto const blink = _terminal->blinkState();
    auto const rapidBlink = _terminal->rapidBlinkState();

    return makeColors(_terminal->colorPalette(),
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

template <CellConcept Cell>
RenderAttributes RenderBufferBuilder<Cell>::createRenderAttributes(
    CellLocation gridPosition, GraphicsAttributes graphicsAttributes) const noexcept
{
    auto const [fg, bg] = makeColorsForCell(gridPosition,
                                            graphicsAttributes.flags,
                                            graphicsAttributes.foregroundColor,
                                            graphicsAttributes.backgroundColor);
    auto renderAttributes = RenderAttributes {};
    renderAttributes.foregroundColor = fg;
    renderAttributes.backgroundColor = bg;
    renderAttributes.decorationColor = CellUtil::makeUnderlineColor(
        _terminal->colorPalette(), fg, graphicsAttributes.underlineColor, graphicsAttributes.flags);
    renderAttributes.flags = graphicsAttributes.flags;
    renderAttributes.lineFlags = _currentLineFlags;
    return renderAttributes;
}

template <CellConcept Cell>
RenderLine RenderBufferBuilder<Cell>::createRenderLine(TrivialLineBuffer const& lineBuffer,
                                                       LineOffset lineOffset) const
{
    auto const pos = CellLocation { .line = lineOffset, .column = ColumnOffset(0) };
    auto const gridPosition = _terminal->viewport().translateScreenToGridCoordinate(pos);
    auto renderLine = RenderLine {};
    renderLine.lineOffset = lineOffset;
    renderLine.usedColumns = lineBuffer.usedColumns;
    renderLine.displayWidth = _terminal->pageSize().columns;
    renderLine.text = lineBuffer.text.view();
    renderLine.textAttributes = createRenderAttributes(gridPosition, lineBuffer.textAttributes);
    renderLine.fillAttributes = createRenderAttributes(gridPosition, lineBuffer.fillAttributes);
    renderLine.flags = _currentLineFlags;

    return renderLine;
}

template <CellConcept Cell>
bool RenderBufferBuilder<Cell>::gridLineContainsCursor(LineOffset lineOffset) const noexcept
{
    if (_terminal->currentScreen().cursor().position.line == lineOffset)
        return true;

    if (_cursorPosition && _terminal->inputHandler().mode() != ViMode::Insert)
    {
        auto const viCursor = _terminal->viewport().translateGridToScreenCoordinate(_cursorPosition->line);
        if (viCursor == lineOffset)
            return true;
    }

    return false;
}

template <CellConcept Cell>
void RenderBufferBuilder<Cell>::renderTrivialLine(TrivialLineBuffer const& lineBuffer,
                                                  LineOffset lineOffset,
                                                  LineFlags flags)
{
    // if (lineBuffer.text.size())
    //     std::cout << std::format("Rendering trivial line {:2} 0..{}/{} ({} bytes): \"{}\" (flags: {})\n",
    //                lineOffset.value,
    //                lineBuffer.usedColumns,
    //                lineBuffer.displayWidth,
    //                lineBuffer.text.size(),
    //                lineBuffer.text.view(),
    //                flags);

    // No need to call isCursorLine(lineOffset) because lines containing a cursor are always inflated.
    _useCursorlineColoring = false;
    _currentLineFlags = flags;

    auto const frontIndex = _output->cells.size();

    // Visual selection can alter colors for some columns in this line.
    // In that case, it seems like we cannot just pass it bare over but have to take the slower path.
    // But that should be fine.
    //
    // Testing for the cursor's current line is made because the cursor might be a block cursor,
    // which affects background/foreground color again.
    // We're not testing for cursor shape (which should be done in order to be 100% correct)
    // because it's not really draining performance.
    bool const canRenderViaSimpleLine =
        (!_terminal->isSelected(lineOffset) || !_includeSelection) && !gridLineContainsCursor(lineOffset);

    if (canRenderViaSimpleLine)
    {
        _output->lines.emplace_back(createRenderLine(lineBuffer, lineOffset));
        _lineNr = lineOffset;
        _prevWidth = 0;
        _prevHasCursor = false;
        return;
    }

    auto const textMargin = min(boxed_cast<ColumnOffset>(_terminal->pageSize().columns),
                                ColumnOffset::cast_from(lineBuffer.usedColumns));
    auto const pageColumnsEnd = boxed_cast<ColumnOffset>(_terminal->pageSize().columns);

    // render text
    _searchPatternOffset = 0;
    renderUtf8Text(CellLocation { .line = lineOffset, .column = ColumnOffset(0) },
                   lineBuffer.textAttributes,
                   lineBuffer.text.view(),
                   true);

    // {{{ fill the remaining empty cells
    for (auto columnOffset = textMargin; columnOffset < pageColumnsEnd; ++columnOffset)
    {
        auto const pos = CellLocation { .line = lineOffset, .column = columnOffset };
        auto const gridPosition = _terminal->viewport().translateScreenToGridCoordinate(pos);
        auto renderAttributes = createRenderAttributes(gridPosition, lineBuffer.fillAttributes);
        auto const scale = _currentLineFlags.test(LineFlag::DoubleWidth) ? 2 : 1;

        _output->cells.emplace_back(makeRenderCellExplicit(_terminal->colorPalette(),
                                                           char32_t { 0 },
                                                           lineBuffer.fillAttributes.flags,
                                                           _currentLineFlags,
                                                           renderAttributes.foregroundColor,
                                                           renderAttributes.backgroundColor,
                                                           lineBuffer.fillAttributes.underlineColor,
                                                           _baseLine + lineOffset,
                                                           columnOffset * scale));
    }
    // }}}

    auto const backIndex = _output->cells.size() - 1;

    _output->cells[frontIndex].groupStart = true;
    _output->cells[backIndex].groupEnd = true;
}

template <CellConcept Cell>
template <typename T>
void RenderBufferBuilder<Cell>::matchSearchPattern(T const& cellText)
{
    if (_highlightSearchMatches == HighlightSearchMatches::No)
        return;

    auto const& search = _terminal->search();
    if (search.pattern.empty())
        return;

    auto const searchText = u32string_view(search.pattern.data() + _searchPatternOffset,
                                           search.pattern.size() - _searchPatternOffset);
    auto const isCaseSensitive =
        std::any_of(searchText.begin(), searchText.end(), [](auto ch) { return std::isupper(ch); });

    auto const isFullMatch = [&]() -> bool {
        return !CellUtil::beginsWith(searchText, cellText, isCaseSensitive);
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

    if (_searchPatternOffset < search.pattern.size())
        return; // match incomplete

    // match complete

    auto const offsetIntoFront = _output->cells.size() - _searchPatternOffset;

    auto const isFocusedMatch =
        CellLocationRange {
            .first = _output->cells[offsetIntoFront].position,
            .second = _output->cells.back().position,
        }
            .contains(
                _terminal->viewport().translateGridToScreenCoordinate(_terminal->normalModeCursorPosition()));

    auto highlightColors = [&]() -> CellRGBColorAndAlphaPair {
        // Oh yeah, this can be optimized :)
        if (isFocusedMatch)
        {
            if (_terminal->search().initiatedByDoubleClick)
                return _terminal->colorPalette().wordHighlightCurrent;
            else
                return _terminal->colorPalette().searchHighlightFocused;
        }
        else
        {
            if (_terminal->search().initiatedByDoubleClick)
                return _terminal->colorPalette().wordHighlight;
            else
                return _terminal->colorPalette().searchHighlight;
        }
    }();

    for (size_t i = offsetIntoFront; i < _output->cells.size(); ++i)
    {
        auto& cellAttributes = _output->cells[i].attributes;
        auto const actualColors = RGBColorPair { .foreground = cellAttributes.foregroundColor,
                                                 .background = cellAttributes.backgroundColor };
        auto const searchMatchColors = makeRGBColorPair(actualColors, highlightColors);

        cellAttributes.backgroundColor = searchMatchColors.background;
        cellAttributes.foregroundColor = searchMatchColors.foreground;
    }
    _searchPatternOffset = 0;
}

template <CellConcept Cell>
void RenderBufferBuilder<Cell>::startLine(LineOffset line, LineFlags flags) noexcept
{
    _lineNr = line;
    _currentLineFlags = flags;
    _prevWidth = 0;
    _prevHasCursor = false;

    _useCursorlineColoring = isCursorLine(line);
}

template <CellConcept Cell>
bool RenderBufferBuilder<Cell>::isCursorLine(LineOffset line) const noexcept
{
    return _terminal->inputHandler().mode() != ViMode::Insert && _cursorPosition
           && line
                  == _terminal->viewport()
                         .translateGridToScreenCoordinate(
                             CellLocation { .line = _cursorPosition->line, .column = {} })
                         .line;
}

template <CellConcept Cell>
void RenderBufferBuilder<Cell>::endLine() noexcept
{
    if (!_output->cells.empty())
    {
        _output->cells.back().groupEnd = true;
    }
}

template <CellConcept Cell>
ColumnCount RenderBufferBuilder<Cell>::renderUtf8Text(CellLocation screenPosition,
                                                      GraphicsAttributes textAttributes,
                                                      std::string_view text,
                                                      bool allowMatchSearchPattern)
{
    auto columnCountRendered = ColumnCount(0);

    auto graphemeClusterSegmenter = unicode::utf8_grapheme_segmenter(text);
    for (u32string const& graphemeCluster: graphemeClusterSegmenter)
    {
        auto const gridPosition = _terminal->viewport().translateScreenToGridCoordinate(
            screenPosition + ColumnOffset::cast_from(columnCountRendered));
        auto const [fg, bg] = makeColorsForCell(gridPosition,
                                                textAttributes.flags,
                                                textAttributes.foregroundColor,
                                                textAttributes.backgroundColor);
        auto const width = graphemeClusterWidth(graphemeCluster);
        // std::cout << std::format(" start {}, count {}, bytes {}, grapheme cluster \"{}\"\n",
        //            columnOffset,
        //            width,
        //            unicode::convert_to<char>(u32string_view(graphemeCluster)).size(),
        //            unicode::convert_to<char>(u32string_view(graphemeCluster)));

        auto const scale = _currentLineFlags.test(LineFlag::DoubleWidth) ? 2 : 1;

        _output->cells.emplace_back(makeRenderCellExplicit(
            _terminal->colorPalette(),
            graphemeCluster,
            width,
            textAttributes.flags,
            _currentLineFlags,
            fg,
            bg,
            textAttributes.underlineColor,
            _baseLine + screenPosition.line,
            (screenPosition.column + ColumnOffset::cast_from(columnCountRendered)) * scale));

        // Span filling cells for preciding wide glyphs to get the background color properly painted.
        for (auto i = ColumnCount(1); i < width; ++i)
        {
            _output->cells.emplace_back(makeRenderCellExplicit(
                _terminal->colorPalette(),
                U" ", // {}
                ColumnCount(1),
                textAttributes.flags,
                _currentLineFlags,
                fg,
                bg,
                textAttributes.underlineColor,
                _baseLine + screenPosition.line,
                (screenPosition.column + ColumnOffset::cast_from(columnCountRendered + i)) * scale));
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

template <CellConcept Cell>
bool RenderBufferBuilder<Cell>::tryRenderInputMethodEditor(CellLocation screenPosition,
                                                           CellLocation gridPosition)
{
    // Render IME preeditString if available and screen position matches cursor position.
    if (_cursorPosition && gridPosition == *_cursorPosition && !_inputMethodData.preeditString.empty())
    {
        auto const inputMethodEditorStyles = _terminal->colorPalette().inputMethodEditor;
        auto textAttributes = GraphicsAttributes {};
        textAttributes.foregroundColor = inputMethodEditorStyles.foreground;
        textAttributes.backgroundColor = inputMethodEditorStyles.background;
        textAttributes.flags.enable({ CellFlag::Bold, CellFlag::Underline });

        if (!_output->cells.empty())
            _output->cells.back().groupEnd = true;

        _inputMethodSkipColumns =
            renderUtf8Text(screenPosition, textAttributes, _inputMethodData.preeditString, false);
        if (_inputMethodSkipColumns > ColumnCount(0))
        {
            _output->cursor->position.column += ColumnOffset::cast_from(_inputMethodSkipColumns);
            _output->cells.at(_output->cells.size() - unbox<size_t>(_inputMethodSkipColumns)).groupStart =
                true;
            _output->cells.back().groupEnd = true;
        }
    }

    if (_inputMethodSkipColumns == ColumnCount(0))
        return false;

    // Skipping grid cells that have already been rendered due to IME.
    _inputMethodSkipColumns--;
    return true;
}

template <CellConcept Cell>
void RenderBufferBuilder<Cell>::renderCell(Cell const& screenCell, LineOffset line, ColumnOffset column)
{
    auto const screenPosition = CellLocation { .line = line, .column = column };
    auto const gridPosition = _terminal->viewport().translateScreenToGridCoordinate(screenPosition);

    if (tryRenderInputMethodEditor(screenPosition, gridPosition))
        return;

    auto const [fg, bg] = makeColorsForCell(
        gridPosition, screenCell.flags(), screenCell.foregroundColor(), screenCell.backgroundColor());

    _prevWidth = screenCell.width();
    _prevHasCursor = _cursorPosition && gridPosition == *_cursorPosition;

    auto const displayColumn = _currentLineFlags.test(LineFlag::DoubleWidth) ? column * 2 : column;

    _output->cells.emplace_back(makeRenderCell(_terminal->colorPalette(),
                                               _terminal->hyperlinks(),
                                               screenCell,
                                               _currentLineFlags,
                                               fg,
                                               bg,
                                               _baseLine + line,
                                               displayColumn));

    if (column == ColumnOffset(0))
        _output->cells.back().groupStart = true;

    matchSearchPattern(screenCell);
}

} // namespace vtbackend

#include <vtbackend/cell/CompactCell.h>
template class vtbackend::RenderBufferBuilder<vtbackend::CompactCell>;

#include <vtbackend/cell/SimpleCell.h>
template class vtbackend::RenderBufferBuilder<vtbackend::SimpleCell>;
