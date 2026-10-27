// SPDX-License-Identifier: Apache-2.0

#include <vtbackend/screen/RenderBufferBuilder.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/screen/LinkPresentation.hpp>

#include <core/Utils.hpp>

#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>
#include <libunicode/width.h>

#include <algorithm>
#include <ranges>
#include <vector>

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

    /// Gives @p cell its link decoration: the role the renderer maps to the profile's decorator, and the
    /// colour scheme's hyperlink_decoration colour for it.
    void applyLinkRole(RenderCell& cell, LinkDecorationRole role, ColorPalette const& palette) noexcept
    {
        if (role == LinkDecorationRole::None)
            return;
        cell.linkRole = role;
        cell.attributes.decorationColor = role == LinkDecorationRole::Hover
                                              ? palette.hyperlinkDecoration.hover
                                              : palette.hyperlinkDecoration.normal;
    }

    /// The columns @p span covers, those at @p limit and beyond left out.
    /// @param span A row's part of a detected link.
    /// @param limit How many columns there are to decorate.
    /// @return The columns, left to right.
    [[nodiscard]] auto columnsOf(LinkRowSpan const& span, size_t limit) noexcept
    {
        auto const end = std::min(unbox<size_t>(span.last) + 1, limit);
        return std::views::iota(std::min(unbox<size_t>(span.first), end), end);
    }

    /// Tells whether the terminal still holds @p hyperlink: a link evicted from the cache is dead, nothing
    /// opens it. The lookup refreshes the link in the cache, as drawing it always has.
    bool isHyperlinkLive(HyperlinkStorage const& hyperlinks, HyperlinkId hyperlink) noexcept
    {
        return hyperlinks.cache.tryGet(hyperlink) != nullptr;
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
                                         bool includeSelection,
                                         FrameLinks links,
                                         RenderedDisplay display):
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
    _hoveredHyperlink { links.hoveredHyperlink },
    _linkMode { terminal.settings().linkDecorationMode },
    _followModifiers { terminal.followModifiers() },
    _hoverPointer { terminal.hoverPointer() },
    // The two roles a detected link can take this pass, so that a row's spans each pick one without asking
    // the presentation table.
    _detectedRestRole { presentLink(_linkMode, LinkPointer::Elsewhere, _followModifiers).role },
    _detectedHoverRole { presentLink(_linkMode, _hoverPointer, _followModifiers).role },
    _detectedLinks { _detectedRestRole == LinkDecorationRole::None
                             && _detectedHoverRole == LinkDecorationRole::None
                         ? nullptr
                         : links.detectedLinks },
    _hoveredDetectedLink { links.hoveredDetectedLink },
    _display { display },
    // Once per pass, not once per line: three facts that cannot change while a buffer is being built.
    _contextTintingPossible { terminal.settings().contextTintScope != ContextTintScope::Off
                              && terminal.colorPalette().hasContextTints()
                              && &screen == &terminal.primaryScreen() },
    _blinkOpacity { terminal.blinkState() },
    _rapidBlinkOpacity { terminal.rapidBlinkState() }
{
    output.frameID = terminal.lastFrameID();

    if (_cursorPosition)
        output.cursor = renderCursor();
}

void RenderBufferBuilder::renderDetachedLine(std::vector<RenderCell>& output,
                                             Terminal const& terminal,
                                             Line const& line,
                                             std::span<LinkRowSpan const> detectedLinks,
                                             LineOffset screenRow,
                                             ColumnCount columns)
{
    // A blank row has no materialised cells to read -- the reason Grid::render keeps one off the
    // per-cell path -- and nothing on it to show anyway.
    if (line.isBlank())
        return;

    auto const& palette = terminal.colorPalette();
    auto const reverseVideo = terminal.isModeEnabled(DECMode::ReverseVideo);
    auto const blink = terminal.blinkState();
    auto const rapidBlink = terminal.rapidBlinkState();
    // The header is no hover target -- it takes the pointer itself -- so its links are never under it, and
    // the mode and the held modifiers decide whether they show, as they do in the grid.
    auto const headerLinkRole = presentLink(terminal.settings().linkDecorationMode,
                                            LinkPointer::Elsewhere,
                                            terminal.followModifiers())
                                    .role;
    auto const limit = std::min(unbox<size_t>(line.size()), unbox<size_t>(std::max(columns, ColumnCount(0))));
    // Only what shows: trailing default cells would draw nothing over the band. The wide-glyph check below
    // still measures against the limit, so a glyph that starts before the trimmed end is copied whole.
    auto const shown = std::min(limit, unbox<size_t>(line.trimmedColumns()));
    auto const& storage = line.storage();
    auto const first = output.size();
    output.reserve(first + shown);

    for (auto const column: std::views::iota(size_t { 0 }, shown))
    {
        auto const cell = ConstCellProxy(storage, column);

        // Half a glyph is not a glyph: one that would straddle the limit is dropped whole.
        if (column + std::max<size_t>(1, cell.width()) > limit)
            break;

        // No selection, cursor, cursor line or highlight: those describe the row where it really is,
        // which is still on the page underneath. No tint either: a cell on the page background stays on
        // it, and the header's opaque band shows through.
        auto const [fg, bg] = makeColors(palette,
                                         palette.colorLookupTable,
                                         cell.flags(),
                                         reverseVideo,
                                         cell.foregroundColor(),
                                         cell.backgroundColor(),
                                         /*selected=*/false,
                                         /*isCursor=*/false,
                                         /*isCursorLine=*/false,
                                         /*isHighlighted=*/false,
                                         blink,
                                         rapidBlink,
                                         std::nullopt);

        // The row's own line flags stay behind: the copy is drawn single width and single height.
        output.emplace_back(
            makeRenderCell(palette, cell, LineFlags {}, fg, bg, screenRow, ColumnOffset::cast_from(column)));

        if (auto const hyperlink = cell.hyperlink();
            hyperlink != HyperlinkId {} && isHyperlinkLive(terminal.hyperlinks(), hyperlink))
            applyLinkRole(output.back(), headerLinkRole, palette);
    }

    // Detected links at rest too. Their cells hold no OSC 8 link, which the scan does not read.
    auto const copied = std::span { output }.subspan(first);
    if (headerLinkRole != LinkDecorationRole::None)
        for (auto const& span: detectedLinks)
            for (auto const column: columnsOf(span, copied.size()))
                applyLinkRole(copied[column], headerLinkRole, palette);

    markShapingGroup(copied);
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
    if (!_cursorPosition || !cursorVisible || !isGridLineDrawn(_cursorPosition->line))
        return nullopt;

    // TODO: check if CursorStyle has changed, and update render context accordingly.

    auto constexpr InactiveCursorShape = CursorShape::Rectangle; // TODO configurable
    auto const shape = _terminal->focused() ? _terminal->cursorShape() : InactiveCursorShape;

    // Through the viewport rather than by adding the scroll offset: with folds collapsed the row a grid
    // line is drawn on is not its line plus an offset, and a cursor placed by the old arithmetic would
    // be drawn somewhere other than the cell it is in. A status line's cursor is on the status line's row.
    auto const cursorScreenPosition = CellLocation { .line = _baseLine + screenLineOf(_cursorPosition->line),
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
                                                    _blinkOpacity,
                                                    _rapidBlinkOpacity);
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
    // A status line's cursor is never animated (Terminal::updateCursorMotionAnimation), so it is at rest: a
    // block cursor there is painted by inverting its cell at once.
    auto const animProgress =
        _display == RenderedDisplay::Main ? _terminal->cursorAnimationProgress(*_cursorPosition) : 1.0f;

    return RenderCursor { .position = cursorScreenPosition,
                          .shape = shape,
                          .width = cellWidth,
                          .animationProgress = animProgress,
                          .cursorColor = resolvedCursorColor,
                          .display = _display };
}

CellLocation RenderBufferBuilder::gridPositionOf(CellLocation screenPosition) const
{
    return _display == RenderedDisplay::Main
               ? _terminal->viewport().translateScreenToGridCoordinate(screenPosition)
               : screenPosition;
}

LineOffset RenderBufferBuilder::screenLineOf(LineOffset line) const
{
    return _display == RenderedDisplay::Main ? _terminal->viewport().translateGridToScreenCoordinate(line)
                                             : line;
}

bool RenderBufferBuilder::isGridLineDrawn(LineOffset line) const
{
    if (_display == RenderedDisplay::Main)
        return _terminal->viewport().isLineVisible(line);
    return line >= LineOffset(0) && line < _screen->pageSize().lines.as<LineOffset>();
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

    return renderCell;
}

LinkDecorationRole RenderBufferBuilder::osc8RoleOf(HyperlinkId hyperlink) const noexcept
{
    if (hyperlink == HyperlinkId {})
        return LinkDecorationRole::None;
    if (hyperlink == _lastRoleLink)
        return _lastRole;

    _lastRoleLink = hyperlink;
    if (!isHyperlinkLive(_terminal->hyperlinks(), hyperlink))
        _lastRole = LinkDecorationRole::None;
    else
    {
        auto const pointer = hyperlink == _hoveredHyperlink ? _hoverPointer : LinkPointer::Elsewhere;
        _lastRole = presentLink(_linkMode, pointer, _followModifiers).role;
    }
    return _lastRole;
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
        takesTheMainPagesMarks() && _includeSelection
        && _terminal->isSelected(*_screen,
                                 CellLocation { .line = gridPosition.line, .column = gridPosition.column });
    auto const highlighted = takesTheMainPagesMarks()
                             && _terminal->isHighlighted(
                                 CellLocation { .line = gridPosition.line, .column = gridPosition.column });

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
                      _blinkOpacity,
                      _rapidBlinkOpacity,
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
    auto const gridPosition = gridPositionOf(pos);
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
    // A screen handed no cursor position -- a status line the cursor is not on -- has nothing to mark.
    if (!_cursorPosition)
        return false;

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
    if (screenLineOf(_screen->cursor().position.line) == screenRow)
        return true;

    // In Vi mode the cursor drawn is the Vi one, which sits at its own line.
    return _terminal->inputHandler().mode() != ViMode::Insert
           && screenLineOf(_cursorPosition->line) == screenRow;
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
    auto const gridLine = gridPositionOf(CellLocation { .line = lineOffset, .column = ColumnOffset(0) }).line;

    // A line one link covers takes the per-cell path as well: a RenderLine carries no per-cell decoration. So
    // does a line a detected link decorates; a blank line holds none, and is spared the lookup.
    auto const linkRole = osc8RoleOf(lineBuffer.hyperlink);
    if (lineBuffer.usedColumns != ColumnCount(0))
        prepareLinkOverlay(gridLine);
    else
        _rowLinkRoles.clear();
    auto const isMarked =
        takesTheMainPagesMarks()
        && ((_includeSelection && _terminal->isSelected(gridLine)) || _terminal->isHighlighted(gridLine));
    bool const canRenderViaSimpleLine = !isMarked && !gridLineContainsCursor(lineOffset)
                                        && linkRole == LinkDecorationRole::None && _rowLinkRoles.empty();

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
    renderGridText(CellLocation { .line = lineOffset, .column = ColumnOffset(0) },
                   lineBuffer.textAttributes,
                   textOverride);

    // {{{ fill the remaining empty cells
    for (auto columnOffset = textMargin; columnOffset < pageColumnsEnd; ++columnOffset)
    {
        auto const pos = CellLocation { .line = lineOffset, .column = columnOffset };
        auto const gridPosition = gridPositionOf(pos);
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

    if (linkRole != LinkDecorationRole::None || !_rowLinkRoles.empty())
    {
        // The cells went out one per grid column, left to right, so the k-th is grid column k, wherever
        // DECDWL draws it.
        auto const& palette = _terminal->colorPalette();
        for (auto gridColumn = ColumnOffset(0); auto& cell: std::span(_output->cells).subspan(frontIndex))
        {
            applyLinkRole(cell, linkRoleAt(lineBuffer.hyperlink, gridColumn), palette);
            ++gridColumn;
        }
    }

    auto const backIndex = _output->cells.size() - 1;

    _output->cells[frontIndex].groupStart = true;
    _output->cells[backIndex].groupEnd = true;
}

void RenderBufferBuilder::prepareSearchHighlights(LineOffset gridLine)
{
    _rowSearchHighlights.clear();
    if (_highlightSearchMatches == HighlightSearchMatches::No)
        return;

    auto const& search = _terminal->search();
    if (!search.query || _searchFailure)
        return;

    auto const& grid = _screen->grid();
    auto const columns = unbox(grid.pageSize().columns);

    // Only once the rows leave the segment last asked about: finding a row's segment walks up to its logical
    // line's head, which per row would cost a long line's length per row.
    if (!_searchSegment || gridLine < _searchSegment->first || gridLine > _searchSegment->last)
    {
        auto const segment = searchSegmentAt(grid, gridLine, search.query->segmentCells());
        _searchSegment = segment;
        // Remembered across frames: a slow regex over a visible segment is paid once, not on every frame.
        auto const matches = _terminal->searchMatchMemo().matchesIn(search.query, grid, segment);
        if (!matches)
        {
            _searchFailure = matches.error();
            _searchMatches.clear();
            return;
        }
        auto const cursor = _terminal->normalModeCursorPosition();
        for (auto const& match: *matches)
            _searchMatches.push_back(SearchMatch {
                .first = match.first,
                .last = match.second,
                .highlight = match.contains(cursor) ? SearchHighlight::FocusedMatch : SearchHighlight::Match,
            });
    }

    // A match that ends above this row reaches no row still to come.
    std::erase_if(_searchMatches,
                  [gridLine](SearchMatch const& match) { return match.last.line < gridLine; });

    _rowSearchHighlights.assign(static_cast<size_t>(columns), SearchHighlight::None);
    auto const& line = grid.lineAt(gridLine);
    for (auto const& match: _searchMatches)
    {
        if (match.first.line > gridLine)
            continue;
        auto const from = match.first.line == gridLine ? unbox(match.first.column) : 0;
        auto const to = match.last.line == gridLine ? unbox(match.last.column) : columns - 1;
        for (auto const column: std::views::iota(from, to + 1))
        {
            // Padding and a tall block's lower part are no cells of the match: it stepped over them.
            if (!line.isBlank()
                && line.storage().sgr[static_cast<size_t>(column)].flags.any(
                    CellFlags { CellFlag::WrapPadding } | CellFlag::MulticellContinuation))
                continue;
            auto& highlight = _rowSearchHighlights[static_cast<size_t>(column)];
            highlight = max(highlight, match.highlight);
        }
    }
}

void RenderBufferBuilder::paintSearchHighlight(ColumnOffset column)
{
    auto const index = unbox<size_t>(column);
    if (index >= _rowSearchHighlights.size() || _rowSearchHighlights[index] == SearchHighlight::None)
        return;

    auto const isFocusedMatch = _rowSearchHighlights[index] == SearchHighlight::FocusedMatch;
    auto const isDoubleClick = _terminal->search().origin == SearchOrigin::DoubleClick;
    auto const& palette = _terminal->colorPalette();
    auto const& highlightColors = [&]() -> CellRGBColorAndAlphaPair const& {
        if (isDoubleClick)
            return isFocusedMatch ? palette.wordHighlightCurrent : palette.wordHighlight;
        return isFocusedMatch ? palette.searchHighlightFocused : palette.searchHighlight;
    }();

    auto& cellAttributes = _output->cells.back().attributes;
    auto const actualColors = RGBColorPair { .foreground = cellAttributes.foregroundColor,
                                             .background = cellAttributes.backgroundColor };
    auto const searchMatchColors = makeRGBColorPair(actualColors, highlightColors);
    cellAttributes.backgroundColor = searchMatchColors.background;
    // A fill that sets its own ink is mixed over the cell's own background, which is not always the page --
    // a reverse-video bar, a selection -- so the ink is picked against what the cell actually shows.
    cellAttributes.foregroundColor = std::holds_alternative<RGBColor>(highlightColors.foreground)
                                         ? legibleInkOn(palette, searchMatchColors.background)
                                         : searchMatchColors.foreground;
}

void RenderBufferBuilder::prepareLinkOverlay(LineOffset gridLine)
{
    _rowLinkRoles.clear();
    if (!takesTheMainPagesMarks() || !_detectedLinks || _detectedLinks->empty())
        return;
    for (auto const& span: currentSpansOn(*_detectedLinks, _screen->grid(), gridLine))
    {
        auto const role = span.link == _hoveredDetectedLink ? _detectedHoverRole : _detectedRestRole;
        if (role == LinkDecorationRole::None)
            continue;
        // Only a role that draws something makes an overlay, so that a row whose links show nothing in this
        // mode keeps its batched line.
        if (_rowLinkRoles.empty())
            _rowLinkRoles.assign(unbox<size_t>(_screen->grid().pageSize().columns), LinkDecorationRole::None);
        for (auto const column: columnsOf(span, _rowLinkRoles.size()))
            _rowLinkRoles[column] = role;
    }
}

LinkDecorationRole RenderBufferBuilder::detectedRoleAt(ColumnOffset column) const noexcept
{
    auto const index = unbox<size_t>(column);
    return index < _rowLinkRoles.size() ? _rowLinkRoles[index] : LinkDecorationRole::None;
}

LinkDecorationRole RenderBufferBuilder::linkRoleAt(HyperlinkId hyperlink, ColumnOffset column) const noexcept
{
    auto const role = osc8RoleOf(hyperlink);
    return role != LinkDecorationRole::None ? role : detectedRoleAt(column);
}

void RenderBufferBuilder::startLine(LineOffset line, LineFlags flags, ContextId contextId)
{
    _lineNr = line;
    _currentLineFlags = flags;
    _prevWidth = 0;
    _prevHasCursor = false;

    _useCursorlineColoring = isCursorLine(line);
    _currentContextTint = tintFor(contextId);

    auto const gridLine = gridPositionOf(CellLocation { .line = line, .column = ColumnOffset(0) }).line;
    prepareSearchHighlights(gridLine);
    prepareLinkOverlay(gridLine);
}

bool RenderBufferBuilder::isCursorLine(LineOffset line) const
{
    return _terminal->inputHandler().mode() != ViMode::Insert && _cursorPosition
           && line == screenLineOf(_cursorPosition->line);
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
        auto const gridPosition =
            gridPositionOf(CellLocation { .line = screenPosition.line, .column = column });
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
        auto const gridPosition =
            gridPositionOf(screenPosition + ColumnOffset::cast_from(columnCountRendered));
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
    auto const gridPosition = gridPositionOf(screenPosition);

    if (tryRenderInputMethodEditor(screenPosition, gridPosition))
        return;

    auto const [fg, bg] = makeColorsForCell(
        gridPosition, screenCell.flags(), screenCell.foregroundColor(), screenCell.backgroundColor());

    _prevWidth = screenCell.width();
    _prevHasCursor = _cursorPosition && gridPosition == *_cursorPosition;

    _output->cells.emplace_back(makeRenderCell(_terminal->colorPalette(),
                                               screenCell,
                                               _currentLineFlags,
                                               fg,
                                               bg,
                                               _baseLine + line,
                                               displayColumn(column)));

    // Most cells link nowhere: those cost a few tests, and no link lookup.
    applyLinkRole(
        _output->cells.back(), linkRoleAt(screenCell.hyperlink(), column), _terminal->colorPalette());

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

    paintSearchHighlight(column);
}

} // namespace vtbackend
