
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

    tuple<RGBColor, RGBColor> makeColors(ColorPalette const& _colorPalette,
                                         Cell const& screenCell,
                                         bool _reverseVideo,
                                         bool _selected,
                                         bool _isCursor)
    {
        auto const [fg, bg] = screenCell.makeColors(_colorPalette, _reverseVideo);
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

        return tuple { cursorFg, cursorBg };
    }

} // namespace

template <typename Cell>
RenderBufferBuilder<Cell>::RenderBufferBuilder(Terminal const& _terminal, RenderBuffer& _output):
    output { _output }, terminal { _terminal }
{
    output.clear();
    output.frameID = _terminal.lastFrameID();
    output.cursor = renderCursor();
}

template <typename Cell>
optional<RenderCursor> RenderBufferBuilder<Cell>::renderCursor() const
{
    auto const cursorPosition = terminal.realCursorPosition();
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
void RenderBufferBuilder<Cell>::operator()(Cell const& screenCell, LineOffset _line, ColumnOffset _column)
{
    // clang-format off
    auto const selected = terminal.isSelected( CellLocation { _line - boxed_cast<LineOffset>(terminal.viewport().scrollOffset()), _column });
    auto const pos = CellLocation { _line, _column };
    auto const gridPosition = terminal.viewport().translateScreenToGridCoordinate(pos);
    auto const hasCursor = gridPosition == terminal.realCursorPosition();
    bool const paintCursor =
        (hasCursor || (prevHasCursor && prevWidth == 2))
            && output.cursor.has_value()
            && output.cursor->shape == CursorShape::Block;
    auto const [fg, bg] = makeColors(terminal.colorPalette(), screenCell, reverseVideo, selected, paintCursor);
    // clang-format on

    prevWidth = screenCell.width();
    prevHasCursor = hasCursor;

    auto const cellEmpty = screenCell.empty();
    auto const customBackground = bg != terminal.colorPalette().defaultBackground || !!screenCell.styles();

    bool isNewLine = false;
    if (lineNr != _line)
    {
        isNewLine = true;
        lineNr = _line;
        prevWidth = 0;
        prevHasCursor = false;
        if (!output.screen.empty())
            output.screen.back().groupEnd = true;
    }

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
}

} // namespace terminal

#include <terminal/Cell.h>
template class terminal::RenderBufferBuilder<terminal::Cell>;
