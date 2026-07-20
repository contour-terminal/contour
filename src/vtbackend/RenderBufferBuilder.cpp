// SPDX-License-Identifier: Apache-2.0

#include <vtbackend/RenderBufferBuilder.h>

#include <vtbackend/CellUtil.h>
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>

#include <crispy/utils.h>

#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>

#include <algorithm>

using namespace std;

namespace vtbackend
{

namespace
{
    /// Width of a grapheme cluster, in columns.
    ///
    /// This used to be a private copy of the rule that handled VS16 but not VS15, which meant the
    /// renderer and the grid could disagree about how wide the same cluster was. Both now ask
    /// libunicode.
    ColumnCount graphemeClusterWidth(std::u32string_view cluster) noexcept
    {
        assert(!cluster.empty());
        return ColumnCount::cast_from(unicode::grapheme_cluster_width(cluster));
    }

    /// The preedit string as one codepoint per grid COLUMN, which is the shape the bidi layout takes a
    /// line in: a wide cluster repeats its codepoint across the columns it covers, exactly as
    /// Line::codepointsPerColumn() presents grid content.
    [[nodiscard]] std::u32string preeditCodepointsPerColumn(std::string_view preedit)
    {
        auto result = std::u32string {};
        for (std::u32string const& cluster: unicode::utf8_grapheme_segmenter(preedit))
        {
            auto const width =
                std::max(ColumnCount(1), ColumnCount::cast_from(unicode::grapheme_cluster_width(cluster)));
            result.append(unbox<size_t>(width), cluster.empty() ? U' ' : cluster[0]);
        }
        return result;
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
        return RGBColorPair { .foreground = mixColor(actualColors.foreground,
                                                     makeRGBColor(actualColors, configuredColor.foreground),
                                                     configuredColor.foregroundAlpha),
                              .background = mixColor(actualColors.background,
                                                     makeRGBColor(actualColors, configuredColor.background),
                                                     configuredColor.backgroundAlpha) }
            .distinct();
    }

    RGBColorPair makeColors(ColorPalette const& colorPalette,
                            ColorLookupTable colorLookupTable,
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
        auto sgrColors = CellUtil::makeColors(colorPalette,
                                              colorLookupTable,
                                              cellFlags,
                                              reverseVideo,
                                              foregroundColor,
                                              backgroundColor,
                                              blink,
                                              rapidBlink);

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

        return mixColor(selectionColors, cursorColor, 0.25f).distinct();
    }

} // namespace

RenderBufferBuilder::RenderBufferBuilder(Terminal const& terminal,
                                         Screen const& screen,
                                         RenderBuffer& output,
                                         LineOffset base,
                                         bool theReverseVideo,
                                         ColorLookupTable colorLookupTable,
                                         HighlightSearchMatches highlightSearchMatches,
                                         InputMethodData inputMethodData,
                                         optional<CellLocation> theCursorPosition,
                                         bool includeSelection):
    _output { &output },
    _terminal { &terminal },
    _screen { &screen },
    _cursorPosition { theCursorPosition },
    _baseLine { base },
    _reverseVideo { theReverseVideo },
    _colorLookupTable { colorLookupTable },
    _highlightSearchMatches { highlightSearchMatches },
    _inputMethodData { std::move(inputMethodData) },
    _includeSelection { includeSelection }
{
    output.frameID = terminal.lastFrameID();

    if (_cursorPosition)
        output.cursor = renderCursor();
}

optional<RenderCursor> RenderBufferBuilder::renderCursor() const
{
    if (!_cursorPosition || !_terminal->cursorCurrentlyVisible()
        || !_terminal->viewport().isLineVisible(_cursorPosition->line))
        return nullopt;

    // TODO: check if CursorStyle has changed, and update render context accordingly.

    auto constexpr InactiveCursorShape = CursorShape::Rectangle; // TODO configurable
    auto const shape = _terminal->focused() ? _terminal->cursorShape() : InactiveCursorShape;

    // The cursor sits OVER a character rather than between two, so it simply follows the character
    // it is on: its logical column is mapped through the same permutation the cells took. What
    // changes with direction is which edge of the cell a bar or underline is drawn on, which the
    // renderer decides from RenderCursor::direction.
    auto const& bidiLayout = _terminal->bidiLayoutAt(_cursorPosition->line);
    auto const visualCursorColumn = bidiLayout.visualColumnAt(_cursorPosition->column);

    auto const cursorScreenPosition =
        CellLocation { .line = _baseLine + _cursorPosition->line
                               + boxed_cast<LineOffset>(_terminal->viewport().scrollOffset()),
                       .column = visualCursorColumn };

    auto const cellWidth = _screen->cellWidthAt(*_cursorPosition);

    // Resolve cursor color from the cell under the cursor, using the same logic as makeColorsForCell
    // for Block cursor inversion. This ensures the cursor color reflects actual cell content rather
    // than only palette defaults.
    auto const resolvedCursorColor = [&]() -> RGBColor {
        auto const& colorPalette = _terminal->colorPalette();
        auto const cellFlags = _screen->cellFlagsAt(*_cursorPosition);
        // Read through the screen being rendered, not the terminal's current one: a status line and
        // a non-displayed page render through this same builder.
        auto const cellFg = _screen->cellForegroundColorAt(*_cursorPosition);
        auto const cellBg = _screen->cellBackgroundColorAt(*_cursorPosition);
        auto const sgrColors = CellUtil::makeColors(colorPalette,
                                                    _colorLookupTable,
                                                    cellFlags,
                                                    _reverseVideo,
                                                    cellFg,
                                                    cellBg,
                                                    _terminal->blinkState(),
                                                    _terminal->rapidBlinkState());
        if (holds_alternative<CellForegroundColor>(colorPalette.cursor.color))
            return sgrColors.foreground;
        if (holds_alternative<CellBackgroundColor>(colorPalette.cursor.color))
            return sgrColors.background;
        return get<RGBColor>(colorPalette.cursor.color);
    }();

    // Pre-compute animation progress so that makeColorsForCell() sees the correct value
    // during cell rendering. Without this, animationProgress defaults to 1.0f and the
    // Block cursor cell inversion fires at the destination while the CursorRenderer also
    // draws an animated cursor at the interpolated position — producing a double/stretched cursor.
    auto const animProgress = _terminal->cursorAnimationProgress(*_cursorPosition);

    return RenderCursor { .position = cursorScreenPosition,
                          .shape = shape,
                          .width = cellWidth,
                          .animationProgress = animProgress,
                          .cursorColor = resolvedCursorColor,
                          .direction = (bidiLayout.levelAt(_cursorPosition->column) & 1) != 0
                                           ? unicode::Bidi_Direction::Right_To_Left
                                           : unicode::Bidi_Direction::Left_To_Right,
                          .mixedDirection = bidiLayout.mixedDirection };
}

RenderCell RenderBufferBuilder::makeRenderCellExplicit(ColorPalette const& colorPalette,
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

    // The renderer places a glyph at the column its cluster names, and clusters are counted in
    // GlyphSizing::columns -- so a wide grapheme cluster must say so here too, or everything after it
    // in a status line or an IME preedit string is drawn one column too far left.
    renderCell.sizing.columns = std::max<uint8_t>(1, unbox<uint8_t>(width));
    renderCell.codepoints = std::move(graphemeCluster);
    return renderCell;
}

RenderCell RenderBufferBuilder::makeRenderCellExplicit(ColorPalette const& colorPalette,
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

RenderCell RenderBufferBuilder::makeRenderCell(ColorPalette const& colorPalette,
                                               HyperlinkStorage const& hyperlinks,
                                               ConstCellProxy screenCell,
                                               LineFlags lineFlags,
                                               RGBColor fg,
                                               RGBColor bg,
                                               LineOffset line,
                                               ColumnOffset column)
{
    RenderCell renderCell;
    renderCell.attributes.backgroundColor = bg;
    renderCell.attributes.foregroundColor = fg;
    renderCell.attributes.decorationColor =
        CellUtil::makeUnderlineColor(colorPalette, fg, screenCell.underlineColor(), screenCell.flags());
    renderCell.attributes.flags = screenCell.flags();
    renderCell.attributes.lineFlags = lineFlags;
    renderCell.position.line = line;
    renderCell.position.column = column;
    renderCell.width = screenCell.width();
    renderCell.sizing.scale = screenCell.textScale();
    renderCell.sizing.columns = std::max<uint8_t>(1, screenCell.width());

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

RGBColorPair RenderBufferBuilder::makeColorsForCell(CellLocation gridPosition,
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
        && _terminal->isSelected(*_screen,
                                 CellLocation { .line = gridPosition.line, .column = gridPosition.column });
    auto const highlighted =
        _terminal->isHighlighted(CellLocation { .line = gridPosition.line, .column = gridPosition.column });
    auto const blink = _terminal->blinkState();
    auto const rapidBlink = _terminal->rapidBlinkState();

    return makeColors(_terminal->colorPalette(),
                      _colorLookupTable,
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

RenderAttributes RenderBufferBuilder::createRenderAttributes(
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

RenderLine RenderBufferBuilder::createRenderLine(TrivialLineBuffer const& lineBuffer,
                                                 LineOffset lineOffset,
                                                 std::u32string_view textOverride) const
{
    auto const pos = CellLocation { .line = lineOffset, .column = ColumnOffset(0) };
    auto const gridPosition = _terminal->viewport().translateScreenToGridCoordinate(pos);
    auto renderLine = RenderLine {};
    renderLine.lineOffset = lineOffset;
    renderLine.usedColumns = lineBuffer.usedColumns;
    renderLine.displayWidth = _terminal->pageSize().columns;
    renderLine.text = std::u32string(textOverride);
    renderLine.textAttributes = createRenderAttributes(gridPosition, lineBuffer.textAttributes);
    renderLine.fillAttributes = createRenderAttributes(gridPosition, lineBuffer.fillAttributes);
    renderLine.flags = _currentLineFlags;

    // A trivial line is uniform in SGR, so permuting its codepoints leaves it a single shaping
    // group -- the reorder is a string permutation and nothing else has to change.
    auto const& layout = _terminal->bidiLayoutAt(gridPosition.line);
    renderLine.paragraphDirection = layout.paragraphDirection;
    if (!layout.identity)
    {
        auto reordered = std::u32string(renderLine.text.size(), U' ');
        for (size_t visual = 0; visual < renderLine.text.size(); ++visual)
        {
            auto const logical = unbox<size_t>(layout.logicalColumnAt(ColumnOffset::cast_from(visual)));
            if (logical < renderLine.text.size())
                reordered[visual] = renderLine.text[logical];
        }
        renderLine.text = std::move(reordered);
        renderLine.visuallyReordered = true;
    }

    return renderLine;
}

bool RenderBufferBuilder::gridLineContainsCursor(LineOffset lineOffset) const noexcept
{
    // The cursor of the screen being RENDERED. Asking the current screen reports the main cursor's
    // line while rendering a status line, forcing an unrelated line off the trivial fast path on
    // every frame and drawing the cursor row in the wrong place.
    if (_screen->cursor().position.line == lineOffset)
        return true;

    if (_cursorPosition && _terminal->inputHandler().mode() != ViMode::Insert)
    {
        auto const viCursor = _terminal->viewport().translateGridToScreenCoordinate(_cursorPosition->line);
        if (viCursor == lineOffset)
            return true;
    }

    return false;
}

void RenderBufferBuilder::renderTrivialLine(TrivialLineBuffer const& lineBuffer,
                                            LineOffset lineOffset,
                                            LineFlags flags,
                                            std::u32string_view textOverride)
{
    // if (lineBuffer.text.size())
    //     std::cout << std::format("Rendering trivial line {:2} 0..{}/{} ({} bytes): \"{}\" (flags: {})\n",
    //                lineOffset.value,
    //                lineBuffer.usedColumns,
    //                lineBuffer.displayWidth,
    //                lineBuffer.text.size(),
    //                lineBuffer.text.view(),
    //                flags);

    // A trivial line can now sit under the (vi) cursor after the AoS→SoA migration — a plain-text
    // line with uniform SGR stays trivial even when the normal-mode cursor is on it. So the
    // cursorline decision must be made here too, exactly as startLine() does for inflated lines;
    // hard-coding it to false (the old invariant "cursor lines are always inflated") dropped the
    // current-line highlight on plain-text lines.
    _useCursorlineColoring = isCursorLine(lineOffset);
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
    // A vi yank/motion highlight range (like a selection) recolors part of the line, so a trivial
    // line intersecting it must drop to the per-cell path where makeColorsForCell() applies the
    // yankHighlight.
    //
    // BOTH the selection and the highlight live in grid coordinates, so this screen line is
    // translated once and the translated value used for both. Passing the untranslated line to the
    // selection test asked about the wrong line whenever the viewport was scrolled back, which sent
    // selected trivial lines down the fast path and left them unhighlighted -- while the per-cell
    // test right beside it (makeColorsForCell) had the coordinates right all along.
    auto const gridLine =
        _terminal->viewport()
            .translateScreenToGridCoordinate(CellLocation { .line = lineOffset, .column = ColumnOffset(0) })
            .line;
    bool const canRenderViaSimpleLine = (!_terminal->isSelected(gridLine) || !_includeSelection)
                                        && !gridLineContainsCursor(lineOffset)
                                        && !_terminal->isHighlighted(gridLine);

    if (canRenderViaSimpleLine)
    {
        _output->lines.emplace_back(createRenderLine(lineBuffer, lineOffset, textOverride));
        _lineNr = lineOffset;
        _prevWidth = 0;
        _prevHasCursor = false;
        return;
    }

    auto const textMargin = min(boxed_cast<ColumnOffset>(_terminal->pageSize().columns),
                                ColumnOffset::cast_from(lineBuffer.usedColumns));
    auto const pageColumnsEnd = boxed_cast<ColumnOffset>(_terminal->pageSize().columns);

    // render text — fallback path for selection/cursor, convert u32 to UTF-8
    _searchPatternOffset = 0;
    auto const utf8Text =
        textOverride.empty() ? std::string(lineBuffer.text.view()) : unicode::convert_to<char>(textOverride);
    renderUtf8Text(CellLocation { .line = lineOffset, .column = ColumnOffset(0) },
                   lineBuffer.textAttributes,
                   utf8Text,
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

    // This fallback path emits cells directly and is never bracketed by startLine()/endLine(), so it
    // has to run the visual reorder itself. It is reached whenever the line carries the cursor, a
    // selection or a highlight -- which is to say, on the line the user is most likely looking at.
    _lineNr = lineOffset;
    _lineStartCellIndex = frontIndex;
    finishLineCells();

    auto const backIndex = _output->cells.size() - 1;

    _output->cells[frontIndex].groupStart = true;
    _output->cells[backIndex].groupEnd = true;
}

template <typename T>
void RenderBufferBuilder::matchSearchPattern(T const& cellText)
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

    if constexpr (std::is_same_v<Cell, T> || std::is_same_v<CellProxy, T>)
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

void RenderBufferBuilder::startLine(LineOffset line, LineFlags flags) noexcept
{
    _lineNr = line;
    _currentLineFlags = flags;
    _prevWidth = 0;
    _prevHasCursor = false;
    _lineStartCellIndex = _output->cells.size();

    _useCursorlineColoring = isCursorLine(line);
}

bool RenderBufferBuilder::isCursorLine(LineOffset line) const noexcept
{
    return _terminal->inputHandler().mode() != ViMode::Insert && _cursorPosition
           && line
                  == _terminal->viewport()
                         .translateGridToScreenCoordinate(
                             CellLocation { .line = _cursorPosition->line, .column = {} })
                         .line;
}

void RenderBufferBuilder::endLine() noexcept
{
    finishLineCells();

    if (!_output->cells.empty())
    {
        _output->cells.back().groupEnd = true;
    }
}

void RenderBufferBuilder::finishLineCells()
{
    reorderLineCells();
    overlayInputMethodPreedit();
}

void RenderBufferBuilder::reorderLineCells()
{
    auto const gridLine =
        _terminal->viewport().translateScreenToGridCoordinate(CellLocation { .line = _lineNr }).line;
    auto const& layout = _terminal->bidiLayoutAt(gridLine);

    auto const first = _lineStartCellIndex;
    auto const count = _output->cells.size() - first;
    if (count == 0)
        return;

    // On the identity path every level is zero, which is already RenderCell::bidiLevel's default --
    // so writing it back costs one call and one store per cell per frame to change nothing. On an
    // all-ASCII page that is every cell on the screen, so the early-out is the fast path.
    if (layout.identity)
        return;

    // The level rides along even where no permutation happens, because a renderer uses it to decide
    // mirroring and shaping direction, not only ordering.
    for (size_t i = 0; i < count; ++i)
        _output->cells[first + i].bidiLevel = layout.levelAt(ColumnOffset::cast_from(i));

    auto reordered = std::vector<RenderCell> {};
    reordered.reserve(count);
    for (size_t visual = 0; visual < count; ++visual)
    {
        auto const logical = unbox<size_t>(layout.logicalColumnAt(ColumnOffset::cast_from(visual)));
        if (logical >= count)
            continue;
        reordered.push_back(_output->cells[first + logical]);
        // The cell keeps its own attributes and codepoints but is drawn where the algorithm puts it.
        reordered.back().position.column = ColumnOffset::cast_from(visual);
    }

    // A shaping group may not straddle a change of direction, so the boundaries are re-derived from
    // the levels rather than carried over from logical order.
    for (size_t i = 0; i < reordered.size(); ++i)
    {
        auto const levelChanged = i == 0 || reordered[i].bidiLevel != reordered[i - 1].bidiLevel;
        reordered[i].groupStart = levelChanged;
        if (i > 0 && levelChanged)
            reordered[i - 1].groupEnd = true;
    }

    std::ranges::copy(reordered, _output->cells.begin() + static_cast<ptrdiff_t>(first));
}

ColumnCount RenderBufferBuilder::renderUtf8Text(CellLocation screenPosition,
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

void RenderBufferBuilder::overlayInputMethodPreedit()
{
    if (_inputMethodData.preeditString.empty() || !_output->cursor)
        return;

    // The cursor of the screen being RENDERED, for the same reason gridLineContainsCursor() asks it:
    // a status line has a cursor of its own.
    if (_screen->cursor().position.line != _lineNr)
        return;

    // Lay the preedit out with the surrounding paragraph's base direction, so that Arabic or Hebrew
    // being composed reads the way it will once committed. Its own strong characters still decide
    // their runs, so the base only settles the neutrals -- which is why a right-to-left preedit reads
    // correctly even while the shell's line around it runs left to right.
    auto const gridLine =
        _terminal->viewport().translateScreenToGridCoordinate(CellLocation { .line = _lineNr }).line;
    auto const perColumn = preeditCodepointsPerColumn(_inputMethodData.preeditString);
    if (perColumn.empty())
        return;

    auto const preeditInput = BidiLineInput { .text = perColumn, .continuesParagraph = false };
    auto const preeditLayout = computeBidiPageLayout(std::span(&preeditInput, 1),
                                                     _terminal->bidiLayoutAt(gridLine).paragraphDirection);
    auto const& layout = preeditLayout.lineAt(0);

    // Composing text is marked out from committed text, so that what is still provisional is obvious.
    auto const styles = _terminal->colorPalette().inputMethodEditor;
    auto const flags = CellFlags {} | CellFlag::Bold | CellFlag::Underline;

    // The cells below have already been permuted, so the cursor's visual column indexes them
    // directly. The block itself grows rightward from there whichever way its text runs; only the
    // text inside it is reordered.
    auto const first = _lineStartCellIndex;
    auto const available = _output->cells.size() - first;
    auto const startColumn = unbox<size_t>(_output->cursor->position.column);
    auto const width = std::min(perColumn.size(), available > startColumn ? available - startColumn : 0);

    for (size_t visual = 0; visual < width; ++visual)
    {
        auto const logical = unbox<size_t>(layout.logicalColumnAt(ColumnOffset::cast_from(visual)));
        auto const codepoint = logical < perColumn.size() ? perColumn[logical] : U' ';
        auto const column = ColumnOffset::cast_from(startColumn + visual);

        auto& cell = _output->cells[first + startColumn + visual];
        cell = makeRenderCellExplicit(_terminal->colorPalette(),
                                      std::u32string(1, codepoint),
                                      ColumnCount(1),
                                      flags,
                                      _currentLineFlags,
                                      styles.foreground,
                                      styles.background,
                                      styles.foreground,
                                      _baseLine + _lineNr,
                                      column);
        cell.bidiLevel = layout.levelAt(ColumnOffset::cast_from(logical));
    }

    if (width == 0)
        return;

    // The preedit is its own shaping run: it neither continues the grid text before it nor runs into
    // whatever follows.
    _output->cells[first + startColumn].groupStart = true;
    _output->cells[first + startColumn + width - 1].groupEnd = true;
    if (startColumn > 0)
        _output->cells[first + startColumn - 1].groupEnd = true;

    // The caret belongs after what has been composed so far, which is where the next character lands.
    _output->cursor->position.column += ColumnOffset::cast_from(width);
}

void RenderBufferBuilder::renderCell(ConstCellProxy screenCell, LineOffset line, ColumnOffset column)
{
    auto const screenPosition = CellLocation { .line = line, .column = column };
    auto const gridPosition = _terminal->viewport().translateScreenToGridCoordinate(screenPosition);

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

    // A row that a tall block reaches down into carries no text of its own, so nothing would be drawn
    // there and the block would exist only as long as its HEAD row was on screen -- scroll the head
    // above the viewport and the whole block vanished instead of being clipped. Give such a row the
    // head's glyph and tell the renderer which band of the block it is; the renderer clips the raster
    // to that row. Only the block's leftmost column on this row draws: the rest are covered by it,
    // exactly as a wide glyph's continuation columns are.
    if (screenCell.isFlagEnabled(CellFlag::MulticellContinuation))
    {
        // The screen being rendered, NOT the terminal's current one: a status line or a page other
        // than the cursor's is rendered through this same builder, and re-resolving would read the
        // block out of an unrelated screen -- drawing the wrong glyph, or missing a block entirely.
        auto const& screen = *_screen;
        if (auto const block = screen.multicellBlockAt(gridPosition);
            block && block->origin.column == gridPosition.column && block->origin.line < gridPosition.line)
        {
            auto const head = screen.at(block->origin);
            auto& emitted = _output->cells.back();
            emitted.codepoints = head.codepoints();
            emitted.width = head.width();
            emitted.sizing.scale = head.textScale();
            emitted.sizing.columns = std::max<uint8_t>(1, head.width());
            emitted.sizing.band = static_cast<uint8_t>(unbox(gridPosition.line) - unbox(block->origin.line));
        }
    }

    if (column == ColumnOffset(0))
        _output->cells.back().groupStart = true;

    // Every block is its own shaping group. Neighbouring blocks share a sizing, so the grouper would
    // otherwise run them together -- and the renderer, handed one group holding several blocks'
    // clusters, has no way to tell where one block's glyphs end and the next one's begin. Shaping
    // advances cannot answer it either: a Devanagari conjunct is several glyphs with advances of
    // their own inside a SINGLE cell.
    if (!_output->cells.back().sizing.scale.isOrdinary())
    {
        _output->cells.back().groupStart = true;
        _output->cells.back().groupEnd = true;
    }

    matchSearchPattern(screenCell);
}

} // namespace vtbackend
