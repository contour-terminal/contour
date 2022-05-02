
#include <terminal/Color.h>
#include <terminal/ColorPalette.h>
#include <terminal/RenderBufferBuilder.h>

#include <tuple>

using namespace std;

namespace terminal
{

namespace
{
    constexpr RGBColor makeRGBColor(RGBColor fg, RGBColor bg, CellRGBColor cellColor) noexcept
    {
        if (holds_alternative<CellForegroundColor>(cellColor))
            return fg;
        if (holds_alternative<CellBackgroundColor>(cellColor))
            return bg;
        return get<RGBColor>(cellColor);
    }

    constexpr RGBColor average(RGBColor a, RGBColor b) noexcept
    {
        return RGBColor(static_cast<uint8_t>((a.red + b.red) / 2),
                        static_cast<uint8_t>((a.green + b.green) / 2),
                        static_cast<uint8_t>((a.blue + b.blue) / 2));
    }

    tuple<RGBColor, RGBColor> makeColors(ColorPalette const& _colorPalette,
                                         CellFlags _cellFlags,
                                         bool _reverseVideo,
                                         Color foregroundColor,
                                         Color backgroundColor,
                                         bool _selected,
                                         bool _isCursor)
    {
        auto const [fg, bg] =
            makeColors(_colorPalette, _cellFlags, _reverseVideo, foregroundColor, backgroundColor);
        if (!_selected && !_isCursor)
            return tuple { fg, bg };

        auto const [selectionFg, selectionBg] =
            [](auto fg, auto bg, bool selected, ColorPalette const& colors) -> tuple<RGBColor, RGBColor> {
            auto const a = colors.selectionForeground.value_or(bg);
            auto const b = colors.selectionBackground.value_or(fg);
            if (selected)
                return tuple { a, b };
            else
                return tuple { b, a };
        }(fg, bg, _selected, _colorPalette);
        if (!_isCursor)
            return tuple { selectionFg, selectionBg };

        auto const cursorFg = makeRGBColor(selectionFg, selectionBg, _colorPalette.cursor.textOverrideColor);
        auto const cursorBg = makeRGBColor(selectionFg, selectionBg, _colorPalette.cursor.color);

        if (_selected)
            return tuple { average(selectionFg, cursorFg), average(selectionFg, cursorFg) };

        return tuple { cursorFg, cursorBg };
    }

} // namespace

template <typename Cell>
RenderBufferBuilder<Cell>::RenderBufferBuilder(Terminal const& _terminal, RenderBuffer& _output):
    output { _output },
    terminal { _terminal },
    cursorPosition { _terminal.inputHandler().mode() == ViMode::Insert
                         ? _terminal.realCursorPosition()
                         : _terminal.state().viCommands.cursorPosition }
{
    output.clear();
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

    auto const cellWidth = terminal.currentScreen().cellWithAt(cursorPosition);

    return RenderCursor { cursorScreenPosition, shape, cellWidth };
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
    renderCell.backgroundColor = bg;
    renderCell.foregroundColor = fg;
    renderCell.decorationColor = getUnderlineColor(_colorPalette, flags, fg, ul);
    renderCell.position.line = _line;
    renderCell.position.column = _column;
    renderCell.flags = flags;
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
    renderCell.backgroundColor = bg;
    renderCell.foregroundColor = fg;
    renderCell.decorationColor = screenCell.getUnderlineColor(_colorPalette, fg);
    renderCell.position.line = _line;
    renderCell.position.column = _column;
    renderCell.flags = screenCell.styles();
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
                ? CellFlags::Underline        // TODO: decorationRenderer_.hyperlinkHover()
                : CellFlags::DottedUnderline; // TODO: decorationRenderer_.hyperlinkNormal();
        renderCell.flags |= decoration;       // toCellStyle(decoration);
        renderCell.decorationColor = color;
    }

    return renderCell;
}

template <typename Cell>
std::tuple<RGBColor, RGBColor> RenderBufferBuilder<Cell>::makeColorsForCell(CellLocation gridPosition,
                                                                            CellFlags cellFlags,
                                                                            Color foregroundColor,
                                                                            Color backgroundColor)
{
    auto const hasCursor = gridPosition == cursorPosition;

    // clang-format off
    bool const paintCursor =
        (hasCursor || (prevHasCursor && prevWidth == 2))
            && output.cursor.has_value()
            && output.cursor->shape == CursorShape::Block;
    // clang-format on

    auto const selected = terminal.isSelected(CellLocation { gridPosition.line, gridPosition.column });

    return makeColors(terminal.colorPalette(),
                      cellFlags,
                      reverseVideo,
                      foregroundColor,
                      backgroundColor,
                      selected,
                      paintCursor);
}

template <typename Cell>
void RenderBufferBuilder<Cell>::renderTrivialLine(TriviallyStyledLineBuffer const& lineBuffer,
                                                  LineOffset lineOffset)
{
    // fmt::print("Rendering trivial line {:2} 0..{}/{}: \"{}\"\n",
    //            lineOffset.value,
    //            lineBuffer.text.size(),
    //            lineBuffer.displayWidth,
    //            lineBuffer.text.view());

    auto const frontIndex = output.screen.size();

    auto const textMargin = min(boxed_cast<ColumnOffset>(terminal.pageSize().columns),
                                ColumnOffset::cast_from(lineBuffer.text.size()));
    auto const pageColumnsEnd = boxed_cast<ColumnOffset>(terminal.pageSize().columns);
    for (auto columnOffset = ColumnOffset(0); columnOffset < textMargin; ++columnOffset)
    {
        auto const pos = CellLocation { lineOffset, columnOffset };
        auto const gridPosition = terminal.viewport().translateScreenToGridCoordinate(pos);
        auto const [fg, bg] = makeColorsForCell(gridPosition,
                                                lineBuffer.attributes.styles,
                                                lineBuffer.attributes.foregroundColor,
                                                lineBuffer.attributes.backgroundColor);
        auto const codepoint = static_cast<char32_t>(lineBuffer.text[unbox<size_t>(columnOffset)]);

        lineNr = lineOffset;
        prevWidth = 0;
        prevHasCursor = false;

        output.screen.emplace_back(makeRenderCellExplicit(terminal.colorPalette(),
                                                          codepoint,
                                                          lineBuffer.attributes.styles,
                                                          fg,
                                                          bg,
                                                          lineBuffer.attributes.underlineColor,
                                                          lineOffset,
                                                          columnOffset));
    }

    for (auto columnOffset = textMargin; columnOffset < pageColumnsEnd; ++columnOffset)
    {
        auto const pos = CellLocation { lineOffset, columnOffset };
        auto const gridPosition = terminal.viewport().translateScreenToGridCoordinate(pos);
        auto const [fg, bg] = makeColorsForCell(gridPosition,
                                                lineBuffer.attributes.styles,
                                                lineBuffer.attributes.foregroundColor,
                                                lineBuffer.attributes.backgroundColor);

        output.screen.emplace_back(makeRenderCellExplicit(terminal.colorPalette(),
                                                          char32_t { 0 },
                                                          lineBuffer.attributes.styles,
                                                          fg,
                                                          bg,
                                                          lineBuffer.attributes.underlineColor,
                                                          lineOffset,
                                                          columnOffset));
    }

    auto const backIndex = output.screen.size() - 1;

    output.screen[frontIndex].groupStart = true;
    output.screen[backIndex].groupEnd = true;
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
    if (!output.screen.empty())
    {
        output.screen.back().groupEnd = true;
    }
}

template <typename Cell>
void RenderBufferBuilder<Cell>::renderCell(Cell const& screenCell, LineOffset _line, ColumnOffset _column)
{
    auto const pos = CellLocation { _line, _column };
    auto const gridPosition = terminal.viewport().translateScreenToGridCoordinate(pos);
    auto const [fg, bg] = makeColorsForCell(
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
                output.screen.emplace_back(makeRenderCell(terminal.colorPalette(),
                                                          terminal.state().hyperlinks,
                                                          screenCell,
                                                          fg,
                                                          bg,
                                                          _line,
                                                          _column));
                output.screen.back().groupStart = true;
            }
            break;
        case State::Sequence:
            if (cellEmpty && !customBackground)
            {
                output.screen.back().groupEnd = true;
                state = State::Gap;
            }
            else
            {
                output.screen.emplace_back(makeRenderCell(terminal.colorPalette(),
                                                          terminal.state().hyperlinks,
                                                          screenCell,
                                                          fg,
                                                          bg,
                                                          _line,
                                                          _column));

                if (isNewLine)
                    output.screen.back().groupStart = true;
            }
            break;
    }
    isNewLine = false;
}

} // namespace terminal

#include <terminal/Cell.h>
template class terminal::RenderBufferBuilder<terminal::Cell>;
