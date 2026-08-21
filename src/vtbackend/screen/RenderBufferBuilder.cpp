// SPDX-License-Identifier: Apache-2.0

#include <vtbackend/screen/RenderBufferBuilder.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/grid/CellUtil.hpp>

#include <crispy/Utils.hpp>

#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>
#include <libunicode/width.h>

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
                            float rapidBlink,
                            std::optional<RGBColor> contextTint) noexcept
    {
        auto sgrColors = CellUtil::makeColors(colorPalette,
                                              colorLookupTable,
                                              cellFlags,
                                              reverseVideo,
                                              foregroundColor,
                                              backgroundColor,
                                              blink,
                                              rapidBlink);

        // The OSC 3008 context tint stands in for the PAGE background and for nothing else.
        //
        // Compared against the RESOLVED background rather than against the SGR parameter, which gets
        // reverse video (DECSCNM) and the Inverse attribute right for free: in both, the resolved
        // background comes from the FOREGROUND colour, does not equal the page background, and is
        // therefore left alone. A cell the application coloured itself keeps that colour -- otherwise
        // `ls --color` inside a container comes out repainted, and a hint about who is running would
        // cost the user the output they ran it for.
        //
        // Lowest of all the layers below, deliberately. Every one of them is a TRANSIENT answer to
        // something the user is doing right now -- moving the cursor, dragging a selection, searching
        // -- and a persistent, ambient property of the line must never win over one of those, or the
        // user loses the ability to see what they just selected.
        if (contextTint && sgrColors.background == colorPalette.defaultBackground)
            sgrColors.background = *contextTint;

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
    _includeSelection { includeSelection },
    // Once per pass, not once per line: three facts that cannot change while a buffer is being built.
    _contextTintingPossible { terminal.settings().contextTintScope != ContextTintScope::Off
                              && terminal.colorPalette().hasContextTints()
                              && &screen == &terminal.primaryScreen() }
{
    output.frameID = terminal.lastFrameID();

    if (_cursorPosition)
        output.cursor = renderCursor();
}

std::optional<RGBColor> RenderBufferBuilder::tintFor(ContextId id) const noexcept
{
    if (!_contextTintingPossible)
        return std::nullopt;

    auto const* const record = _terminal->contexts().find(id);
    // A line whose context has aged out of the retained pool renders untinted, which is the same
    // degradation a cell whose hyperlink was evicted already has. A missing context tints nothing;
    // it never tints wrongly.
    return record
               ? _terminal->colorPalette().contextTint(record->type, _terminal->settings().contextTintScope)
               : std::nullopt;
}

optional<RenderCursor> RenderBufferBuilder::renderCursor() const
{
    // When IME composition is active, the cursor must be rendered regardless of blink phase:
    // the IME preedit text is displayed AT the cursor position, so the cursor is its anchor.
    auto const cursorVisible = _terminal->cursorCurrentlyVisible() || !_inputMethodData.preeditString.empty();
    if (!_cursorPosition || !cursorVisible || !_terminal->viewport().isLineVisible(_cursorPosition->line))
        return nullopt;

    // TODO: check if CursorStyle has changed, and update render context accordingly.

    auto constexpr InactiveCursorShape = CursorShape::Rectangle; // TODO configurable
    auto const shape = _terminal->focused() ? _terminal->cursorShape() : InactiveCursorShape;

    // Through the viewport rather than by adding the scroll offset: with folds collapsed the row a grid
    // line is drawn on is not its line plus an offset, and a cursor placed by the old arithmetic would
    // be drawn somewhere other than the cell it is in.
    auto const cursorScreenPosition =
        CellLocation { .line = _baseLine
                               + _terminal->viewport().translateGridToScreenCoordinate(_cursorPosition->line),
                       .column = _cursorPosition->column };

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
                          .cursorColor = resolvedCursorColor };
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
                      rapidBlink,
                      _currentContextTint);
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

    return renderLine;
}

bool RenderBufferBuilder::gridLineContainsCursor(LineOffset screenRow) const
{
    // The status-line screens render through this same builder and are handed no cursor position at
    // all. They have nothing to mark, and the viewport translation below -- which speaks for the main
    // page -- does not speak for them.
    if (!_cursorPosition)
        return false;

    auto const& viewport = _terminal->viewport();

    // BOTH sides in SCREEN rows. A cursor position is a GRID line, and the row it is drawn on is not
    // that line: scrolling shifts it, and with folds collapsed the rows on screen are a non-contiguous
    // selection of grid rows, so the two differ even at scroll offset zero. Comparing the two spaces
    // marked whichever row happened to share the number and left the cursor's OWN row on the trivial
    // path -- which emits no per-cell data, so a block cursor's cell inversion, which is the whole of
    // how a block cursor is drawn, was never produced. The cursor vanished, and reappeared on some
    // unrelated row, changing as the viewport scrolled.
    //
    // The cursor of the screen being RENDERED, not the terminal's current one: asking the latter
    // reports the main cursor's line while a non-displayed page is being built.
    if (viewport.translateGridToScreenCoordinate(_screen->cursor().position.line) == screenRow)
        return true;

    // In Vi mode the cursor drawn is the Vi one, which sits at its own line.
    return _terminal->inputHandler().mode() != ViMode::Insert
           && viewport.translateGridToScreenCoordinate(_cursorPosition->line) == screenRow;
}

void RenderBufferBuilder::renderTrivialLine(TrivialLineBuffer const& lineBuffer,
                                            LineOffset lineOffset,
                                            LineFlags flags,
                                            ContextId contextId,
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
    _currentContextTint = tintFor(contextId);
    _currentLineFlags = flags;

    // Same reason, and the same job startLine() does for a per-cell line: makeColorsForCell reads
    // this pair to extend a block cursor across the second column of a wide glyph, so it describes
    // the cell BEFORE the one being coloured. A new line has no such cell. Left carrying the
    // previous line's values -- which is what happened while only startLine() cleared them -- a line
    // whose predecessor ended in a wide glyph under the cursor gets its first cell, and with it the
    // whole batched RenderLine or the whole fill run, painted in cursor colours.
    _prevWidth = 0;
    _prevHasCursor = false;

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

    // Render the text cell by cell, because a selection or a cursor gives some column a colour of
    // its own. The text comes from the grid, which has already decided where each cell begins, so
    // it goes to the emitter that respects that -- re-deriving the boundaries here is what let the
    // renderer and the grid disagree in GitHub #1752. An empty line has no text and only wants the
    // fill loop below.
    _searchPatternOffset = 0;
    renderGridText(CellLocation { .line = lineOffset, .column = ColumnOffset(0) },
                   lineBuffer.textAttributes,
                   textOverride);

    // {{{ fill the remaining empty cells
    for (auto columnOffset = textMargin; columnOffset < pageColumnsEnd; ++columnOffset)
    {
        auto const pos = CellLocation { .line = lineOffset, .column = columnOffset };
        auto const gridPosition = _terminal->viewport().translateScreenToGridCoordinate(pos);
        auto renderAttributes = createRenderAttributes(gridPosition, lineBuffer.fillAttributes);

        _output->cells.emplace_back(makeRenderCellExplicit(_terminal->colorPalette(),
                                                           char32_t { 0 },
                                                           lineBuffer.fillAttributes.flags,
                                                           _currentLineFlags,
                                                           renderAttributes.foregroundColor,
                                                           renderAttributes.backgroundColor,
                                                           lineBuffer.fillAttributes.underlineColor,
                                                           _baseLine + lineOffset,
                                                           displayColumn(columnOffset)));
    }
    // }}}

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

    // The cached answer for the WHOLE pattern, not one re-derived from `searchText` -- which is only
    // the part still unmatched. The policy is a property of the search, so deriving it from a suffix
    // made it drift mid-match: with "Foo", the suffix left after 'F' carries no uppercase, and the
    // tail matched case-insensitively. Screen::search reads the same field, so highlighting and
    // matching cannot disagree (they did, for every non-ASCII needle: std::isupper is undefined above
    // 0xFF). Read rather than recomputed because this runs once per RENDERED CELL. @see Search.
    auto const isCaseSensitive = search.isCaseSensitive;

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
            if (_terminal->search().origin == SearchOrigin::DoubleClick)
                return _terminal->colorPalette().wordHighlightCurrent;
            else
                return _terminal->colorPalette().searchHighlightFocused;
        }
        else
        {
            if (_terminal->search().origin == SearchOrigin::DoubleClick)
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

void RenderBufferBuilder::startLine(LineOffset line, LineFlags flags, ContextId contextId)
{
    _lineNr = line;
    _currentLineFlags = flags;
    _prevWidth = 0;
    _prevHasCursor = false;

    _useCursorlineColoring = isCursorLine(line);
    _currentContextTint = tintFor(contextId);
}

bool RenderBufferBuilder::isCursorLine(LineOffset line) const
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
    if (!_output->cells.empty())
    {
        _output->cells.back().groupEnd = true;
    }
}

void RenderBufferBuilder::renderGridText(CellLocation screenPosition,
                                         GraphicsAttributes textAttributes,
                                         std::u32string_view text)
{
    // @p text holds one codepoint per column, as Line::trivialBuffer produces it, and each codepoint
    // is measured on its own -- exactly as TextClusterGrouper::renderLine measures the very same
    // string when the line is drawn batched instead. That agreement is the point: a selection is
    // what switches a line between the two, so any difference in how they measure shows up as text
    // moving when it is selected, which is what GitHub #1752 reported.
    //
    // What must NOT happen here is re-segmenting the text into grapheme clusters, as renderUtf8Text
    // does. The cell boundaries are already decided; re-deriving them can fuse two cells' codepoints
    // into one cluster the grid never formed and pull the rest of the line leftwards.
    for (auto column = screenPosition.column; auto const codepoint: text)
    {
        auto const gridPosition = _terminal->viewport().translateScreenToGridCoordinate(
            CellLocation { .line = screenPosition.line, .column = column });
        auto const [fg, bg] = makeColorsForCell(gridPosition,
                                                textAttributes.flags,
                                                textAttributes.foregroundColor,
                                                textAttributes.backgroundColor);

        // A cell always covers at least one column, and covers two when its codepoint is wide. The
        // trivial path is not supposed to carry a wide cell -- writing one fills a continuation cell
        // whose flags break the line's SGR uniformity -- but Screen::clearAndAdvance skips that fill
        // when only one column is left, so a full-width character on the last writable column does
        // reach here. Measuring it the same way the batched path does keeps the two in step.
        auto const width = ColumnCount::cast_from(std::max(1u, unicode::width(codepoint)));

        _output->cells.emplace_back(makeRenderCellExplicit(_terminal->colorPalette(),
                                                           std::u32string(1, codepoint),
                                                           width,
                                                           textAttributes.flags,
                                                           _currentLineFlags,
                                                           fg,
                                                           bg,
                                                           textAttributes.underlineColor,
                                                           _baseLine + screenPosition.line,
                                                           displayColumn(column)));

        // Spacer cells behind a wide glyph, so its background is painted across both columns.
        for (auto i = ColumnCount(1); i < width; ++i)
            _output->cells.emplace_back(makeRenderCellExplicit(_terminal->colorPalette(),
                                                               U" ",
                                                               ColumnCount(1),
                                                               textAttributes.flags,
                                                               _currentLineFlags,
                                                               fg,
                                                               bg,
                                                               textAttributes.underlineColor,
                                                               _baseLine + screenPosition.line,
                                                               displayColumn(column + i.as<ColumnOffset>())));

        column += width.as<ColumnOffset>();
    }

    _lineNr = screenPosition.line;

    // _prevWidth/_prevHasCursor are NOT maintained here. renderTrivialLine clears them for the whole
    // line, and nothing in the loop above sets them, so every cell is coloured against a cleared
    // pair -- which is what the batched path this must agree with does too, having no per-cell state
    // at all. The visible consequence, shared with master's renderUtf8Text: a block cursor sitting
    // on a wide glyph is not extended across that glyph's second column on this path.
}

ColumnCount RenderBufferBuilder::renderUtf8Text(CellLocation screenPosition,
                                                GraphicsAttributes textAttributes,
                                                std::string_view text)
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
            displayColumn(screenPosition.column + ColumnOffset::cast_from(columnCountRendered))));

        // Span filling cells for preceding wide glyphs to get the background color properly painted.
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
                displayColumn(screenPosition.column + ColumnOffset::cast_from(columnCountRendered + i))));
        }

        columnCountRendered += ColumnCount::cast_from(width);
        _lineNr = screenPosition.line;
        _prevWidth = 0;
        _prevHasCursor = false;
    }
    return columnCountRendered;
}

bool RenderBufferBuilder::tryRenderInputMethodEditor(CellLocation screenPosition, CellLocation gridPosition)
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
            renderUtf8Text(screenPosition, textAttributes, _inputMethodData.preeditString);
        if (_inputMethodSkipColumns > ColumnCount(0))
        {
            if (_output->cursor.has_value())
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

void RenderBufferBuilder::renderCell(ConstCellProxy screenCell, LineOffset line, ColumnOffset column)
{
    auto const screenPosition = CellLocation { .line = line, .column = column };
    auto const gridPosition = _terminal->viewport().translateScreenToGridCoordinate(screenPosition);

    if (tryRenderInputMethodEditor(screenPosition, gridPosition))
        return;

    auto const [fg, bg] = makeColorsForCell(
        gridPosition, screenCell.flags(), screenCell.foregroundColor(), screenCell.backgroundColor());

    _prevWidth = screenCell.width();
    _prevHasCursor = _cursorPosition && gridPosition == *_cursorPosition;

    _output->cells.emplace_back(makeRenderCell(_terminal->colorPalette(),
                                               _terminal->hyperlinks(),
                                               screenCell,
                                               _currentLineFlags,
                                               fg,
                                               bg,
                                               _baseLine + line,
                                               displayColumn(column)));

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
