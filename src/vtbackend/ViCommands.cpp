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
#include <vtbackend/Terminal.h>
#include <vtbackend/ViCommands.h>
#include <vtbackend/logging.h>

#include <fmt/format.h>

#include <memory>

namespace terminal
{

using namespace std;

ViCommands::ViCommands(Terminal& theTerminal): terminal { theTerminal }
{
}

void ViCommands::scrollViewport(ScrollOffset delta)
{
    if (delta.value < 0)
        terminal.viewport().scrollDown(boxed_cast<LineCount>(-delta));
    else
        terminal.viewport().scrollUp(boxed_cast<LineCount>(delta));
}

void ViCommands::searchStart()
{
    terminal.state().searchMode.pattern.clear();
    terminal.screenUpdated();
}

void ViCommands::searchDone()
{
    terminal.screenUpdated();
}

void ViCommands::searchCancel()
{
    terminal.state().searchMode.pattern.clear();
    terminal.screenUpdated();
}

bool ViCommands::jumpToNextMatch(unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
    {
        auto startPosition = cursorPosition;
        if (startPosition.column < boxed_cast<ColumnOffset>(terminal.pageSize().columns))
            startPosition.column++;
        else if (cursorPosition.line < boxed_cast<LineOffset>(terminal.pageSize().lines) - 1)
        {
            startPosition.line++;
            startPosition.column = ColumnOffset(0);
        }

        auto const nextPosition = terminal.search(startPosition);
        if (!nextPosition)
            return false;

        moveCursorTo(nextPosition.value());
    }
    return true;
}

bool ViCommands::jumpToPreviousMatch(unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
    {
        auto startPosition = cursorPosition;
        if (startPosition.column != ColumnOffset(0))
            startPosition.column--;
        else if (cursorPosition.line > -boxed_cast<LineOffset>(terminal.currentScreen().historyLineCount()))
        {
            startPosition.line--;
            startPosition.column = boxed_cast<ColumnOffset>(terminal.pageSize().columns) - 1;
        }

        auto const nextPosition = terminal.searchReverse(startPosition);
        if (!nextPosition)
            return false;

        moveCursorTo(nextPosition.value());
    }
    return true;
}

void ViCommands::updateSearchTerm(std::u32string const& text)
{
    if (auto const newLocation = terminal.searchReverse(text, cursorPosition))
        moveCursorTo(newLocation.value());
}

void ViCommands::modeChanged(ViMode mode)
{
    auto _ = crispy::finally { [this, mode]() {
        lastMode = mode;
    } };

    InputLog()("mode changed to {}\n", mode);

    auto const selectFrom = terminal.selector() ? terminal.selector()->from() : cursorPosition;

    switch (mode)
    {
        case ViMode::Insert:
            // Force re-render as viewport & cursor might have changed.
            terminal.setMode(DECMode::VisibleCursor, lastCursorVisible);
            terminal.setCursorShape(lastCursorShape);
            terminal.viewport().forceScrollToBottom();
            terminal.popStatusDisplay();
            terminal.screenUpdated();
            break;
        case ViMode::Normal:
            lastCursorShape = terminal.cursorShape();
            lastCursorVisible = terminal.isModeEnabled(DECMode::VisibleCursor);
            terminal.setMode(DECMode::VisibleCursor, true);

            if (lastMode == ViMode::Insert)
                cursorPosition = terminal.realCursorPosition();
            if (terminal.selectionAvailable())
                terminal.clearSelection();
            terminal.pushStatusDisplay(StatusDisplayType::Indicator);
            terminal.screenUpdated();
            break;
        case ViMode::Visual:
            terminal.setSelector(make_unique<LinearSelection>(
                terminal.selectionHelper(), selectFrom, terminal.selectionUpdatedHelper()));
            terminal.selector()->extend(cursorPosition);
            terminal.pushStatusDisplay(StatusDisplayType::Indicator);
            break;
        case ViMode::VisualLine:
            terminal.setSelector(make_unique<FullLineSelection>(
                terminal.selectionHelper(), selectFrom, terminal.selectionUpdatedHelper()));
            terminal.selector()->extend(cursorPosition);
            terminal.pushStatusDisplay(StatusDisplayType::Indicator);
            terminal.screenUpdated();
            break;
        case ViMode::VisualBlock:
            terminal.setSelector(make_unique<RectangularSelection>(
                terminal.selectionHelper(), selectFrom, terminal.selectionUpdatedHelper()));
            terminal.selector()->extend(cursorPosition);
            terminal.pushStatusDisplay(StatusDisplayType::Indicator);
            terminal.screenUpdated();
            break;
    }

    terminal.inputModeChanged(mode);
}

void ViCommands::reverseSearchCurrentWord()
{
    // auto const oldPos = cursorPosition;
    auto const [wordUnderCursor, range] = terminal.extractWordUnderCursor(cursorPosition);
    assert(range.contains(cursorPosition));
    cursorPosition = range.first;

    updateSearchTerm(wordUnderCursor);
    jumpToPreviousMatch(1);
}

void ViCommands::searchCurrentWord()
{
    auto const [wordUnderCursor, range] = terminal.extractWordUnderCursor(cursorPosition);
    assert(range.contains(cursorPosition));
    cursorPosition = range.second;
    updateSearchTerm(wordUnderCursor);
    jumpToNextMatch(1);
}

void ViCommands::executeYank(ViMotion motion, unsigned count)
{
    switch (motion)
    {
        case ViMotion::Selection: {
            assert(terminal.selector());
            if (lastMode == ViMode::VisualBlock)
                terminal.setHighlightRange(
                    RectangularHighlight { { terminal.selector()->from(), terminal.selector()->to() } });
            else
                terminal.setHighlightRange(
                    LinearHighlight { { terminal.selector()->from(), terminal.selector()->to() } });
            terminal.copyToClipboard(terminal.extractSelectionText());
            terminal.inputHandler().setMode(ViMode::Normal);
            break;
        }
        default: {
            auto const [from, to] = translateToCellRange(motion, count);
            executeYank(from, to);
        }
        break;
    }
}

void ViCommands::executeYank(CellLocation from, CellLocation to)
{
    assert(terminal.inputHandler().mode() == ViMode::Normal);
    assert(!terminal.selector());

    // TODO: ideally keep that selection for about N msecs,
    // such that it'll be visually rendered and the user has a feedback of what's
    // being clipboarded.
    // Maybe via a event API to inform that a non-visual selection
    // has happened and that it can now either be instantly destroyed
    // or delayed (N msecs, configurable),
    terminal.setSelector(
        make_unique<LinearSelection>(terminal.selectionHelper(), from, terminal.selectionUpdatedHelper()));
    terminal.selector()->extend(to);
    auto const text = terminal.extractSelectionText();
    terminal.copyToClipboard(text);
    terminal.clearSelection();
    terminal.setHighlightRange(LinearHighlight { { from, to } });
    terminal.inputHandler().setMode(ViMode::Normal);
    terminal.screenUpdated();
}

void ViCommands::execute(ViOperator op, ViMotion motion, unsigned count)
{
    InputLog()("{}: Executing: {} {} {}\n", terminal.inputHandler().mode(), count, op, motion);
    switch (op)
    {
        case ViOperator::MoveCursor:
            //.
            moveCursor(motion, count);
            break;
        case ViOperator::Yank:
            //.
            executeYank(motion, count);
            break;
        case ViOperator::Paste:
            //.
            terminal.sendPasteFromClipboard(count, false);
            break;
        case ViOperator::ReverseSearchCurrentWord: // TODO: Does this even make sense to have?
            break;
    }
    terminal.screenUpdated();
}

void ViCommands::select(TextObjectScope scope, TextObject textObject)
{
    auto const [from, to] = translateToCellRange(scope, textObject);
    cursorPosition = to;
    InputLog()("{}: Executing: select {} {} [{} .. {}]\n",
               terminal.inputHandler().mode(),
               scope,
               textObject,
               from,
               to);
    terminal.setSelector(
        make_unique<LinearSelection>(terminal.selectionHelper(), from, terminal.selectionUpdatedHelper()));
    terminal.selector()->extend(to);
    terminal.screenUpdated();
}

void ViCommands::yank(TextObjectScope scope, TextObject textObject)
{
    auto const [from, to] = translateToCellRange(scope, textObject);
    cursorPosition = from;
    InputLog()("{}: Executing: yank {} {}\n", terminal.inputHandler().mode(), scope, textObject);
    executeYank(from, to);
    terminal.screenUpdated();
}

void ViCommands::paste(unsigned count)
{
    terminal.sendPasteFromClipboard(count, false);
}

CellLocationRange ViCommands::expandMatchingPair(TextObjectScope scope, char left, char right) const noexcept
{
    auto a = cursorPosition;
    auto b = cursorPosition;

    auto const rightMargin = terminal.pageSize().columns.as<ColumnOffset>() - 1;
    bool const inner = scope == TextObjectScope::Inner;

    while (a.column.value > 0 && !terminal.currentScreen().compareCellTextAt(a, left))
        --a.column;
    if (inner && terminal.currentScreen().compareCellTextAt(a, left))
        ++a.column;

    while (b.column < rightMargin && !terminal.currentScreen().compareCellTextAt(b, right))
        ++b.column;
    if (inner && terminal.currentScreen().compareCellTextAt(b, right))
        --b.column;

    return { a, b };
}

CellLocationRange ViCommands::translateToCellRange(TextObjectScope scope,
                                                   TextObject textObject) const noexcept
{
    auto const gridTop = -terminal.currentScreen().historyLineCount().as<LineOffset>();
    auto const gridBottom = terminal.pageSize().lines.as<LineOffset>() - 1;
    auto const rightMargin = terminal.pageSize().columns.as<ColumnOffset>() - 1;
    auto a = cursorPosition;
    auto b = cursorPosition;
    switch (textObject)
    {
        case TextObject::AngleBrackets: return expandMatchingPair(scope, '<', '>');
        case TextObject::BackQuotes: return expandMatchingPair(scope, '`', '`');
        case TextObject::CurlyBrackets: return expandMatchingPair(scope, '{', '}');
        case TextObject::DoubleQuotes: return expandMatchingPair(scope, '"', '"');
        case TextObject::Paragraph:
            while (a.line > gridTop && !terminal.currentScreen().isLineEmpty(a.line - 1))
                --a.line;
            while (b.line < gridBottom && !terminal.currentScreen().isLineEmpty(b.line))
                ++b.line;
            break;
        case TextObject::RoundBrackets: return expandMatchingPair(scope, '(', ')');
        case TextObject::SingleQuotes: return expandMatchingPair(scope, '\'', '\'');
        case TextObject::SquareBrackets: return expandMatchingPair(scope, '[', ']');
        case TextObject::Word:
            while (a.column.value > 0 && !terminal.currentScreen().isCellEmpty({ a.line, a.column - 1 }))
                --a.column;
            while (b.column < rightMargin && !terminal.currentScreen().isCellEmpty({ b.line, b.column }))
                ++b.column;
            break;
    }
    return { a, b };
}

CellLocationRange ViCommands::translateToCellRange(ViMotion motion, unsigned count) const noexcept
{
    switch (motion)
    {
        case ViMotion::FullLine:
            return { cursorPosition - cursorPosition.column,
                     { cursorPosition.line, terminal.pageSize().columns.as<ColumnOffset>() - 1 } };
        default:
            //.
            return { cursorPosition, translateToCellLocation(motion, count) };
    }
}

CellLocation ViCommands::snapToCell(CellLocation location) const noexcept
{
    while (location.column > ColumnOffset(0) && terminal.currentScreen().compareCellTextAt(location, '\0'))
        --location.column;

    return location;
}

CellLocation ViCommands::snapToCellRight(CellLocation location) const noexcept
{
    auto const rightMargin = ColumnOffset::cast_from(terminal.pageSize().columns - 1);
    while (location.column < rightMargin && terminal.currentScreen().compareCellTextAt(location, '\0'))
        ++location.column;
    return location;
}

CellLocation ViCommands::translateToCellLocation(ViMotion motion, unsigned count) const noexcept
{
    switch (motion)
    {
        case ViMotion::CharLeft: // h
        {
            auto resultPosition = cursorPosition;
            // Jumping left to the next non-empty column (whitespace is not considered empty).
            // This isn't the most efficient implementation, but it's invoked interactively only anyways.
            for (unsigned i = 0; i < count && resultPosition.column > ColumnOffset(0); ++i)
                resultPosition = snapToCell(resultPosition - ColumnOffset(1));
            return resultPosition;
        }
        case ViMotion::CharRight: // l
        {
            auto const cellWidth =
                std::max(uint8_t { 1 }, terminal.currentScreen().cellWidthAt(cursorPosition));
            auto resultPosition = cursorPosition;
            resultPosition.column += ColumnOffset::cast_from(cellWidth);
            return resultPosition;
        }
        case ViMotion::ScreenColumn: // |
            return snapToCell(
                { cursorPosition.line,
                  min(ColumnOffset::cast_from(count), terminal.pageSize().columns.as<ColumnOffset>() - 1) });
        case ViMotion::FileBegin: // gg
            return snapToCell({ LineOffset::cast_from(-terminal.currentScreen().historyLineCount().as<int>()),
                                ColumnOffset(0) });
        case ViMotion::FileEnd: // G
            return snapToCell({ terminal.pageSize().lines.as<LineOffset>() - 1, ColumnOffset(0) });
        case ViMotion::PageTop: // <S-H>
            return snapToCell({ boxed_cast<LineOffset>(-terminal.viewport().scrollOffset())
                                    + *terminal.viewport().scrollOff(),
                                ColumnOffset(0) });
        case ViMotion::PageBottom: // <S-L>
            return snapToCell({ boxed_cast<LineOffset>(-terminal.viewport().scrollOffset())
                                    + boxed_cast<LineOffset>(terminal.pageSize().lines
                                                             - *terminal.viewport().scrollOff() - 1),
                                ColumnOffset(0) });
        case ViMotion::LineBegin: // 0
            return { cursorPosition.line, ColumnOffset(0) };
        case ViMotion::LineTextBegin: // ^
        {
            auto result = CellLocation { cursorPosition.line, ColumnOffset(0) };
            while (result.column < terminal.pageSize().columns.as<ColumnOffset>() - 1
                   && terminal.currentScreen().isCellEmpty(result))
                ++result.column;
            return result;
        }
        case ViMotion::LineDown: // j
            return { min(cursorPosition.line + LineOffset::cast_from(count),
                         terminal.pageSize().lines.as<LineOffset>() - 1),
                     cursorPosition.column };
        case ViMotion::LineEnd: // $
            return { cursorPosition.line, terminal.pageSize().columns.as<ColumnOffset>() - 1 };
        case ViMotion::LineUp: // k
            return { max(cursorPosition.line - LineOffset::cast_from(count),
                         -terminal.currentScreen().historyLineCount().as<LineOffset>()),
                     cursorPosition.column };
        case ViMotion::PageDown:
            return { min(cursorPosition.line + LineOffset::cast_from(terminal.pageSize().lines / 2),
                         terminal.pageSize().lines.as<LineOffset>() - 1),
                     cursorPosition.column };
        case ViMotion::PageUp:
            return { max(cursorPosition.line - LineOffset::cast_from(terminal.pageSize().lines / 2),
                         -terminal.currentScreen().historyLineCount().as<LineOffset>()),
                     cursorPosition.column };
            return cursorPosition
                   - min(cursorPosition.line, LineOffset::cast_from(terminal.pageSize().lines) / 2);
        case ViMotion::ParagraphBackward: // {
        {
            auto const pageTop = -terminal.currentScreen().historyLineCount().as<LineOffset>();
            auto prev = CellLocation { cursorPosition.line, ColumnOffset(0) };
            if (prev.line.value > 0)
                prev.line--;
            auto current = prev;
            while (current.line > pageTop
                   && (!terminal.currentScreen().isLineEmpty(current.line)
                       || terminal.currentScreen().isLineEmpty(prev.line)))
            {
                prev.line = current.line;
                current.line--;
            }
            return snapToCell(current);
        }
        case ViMotion::ParagraphForward: // }
        {
            auto const pageBottom = terminal.pageSize().lines.as<LineOffset>() - 1;
            auto prev = CellLocation { cursorPosition.line, ColumnOffset(0) };
            if (prev.line < pageBottom)
                prev.line++;
            auto current = prev;
            while (current.line < pageBottom
                   && (!terminal.currentScreen().isLineEmpty(current.line)
                       || terminal.currentScreen().isLineEmpty(prev.line)))
            {
                prev.line = current.line;
                current.line++;
            }
            return snapToCell(current);
        }
        case ViMotion::ParenthesisMatching:  // % TODO
        case ViMotion::SearchResultBackward: // N TODO
        case ViMotion::SearchResultForward:  // n TODO
        case ViMotion::WordBackward: {       // b
            auto prev = cursorPosition;
            if (prev.column.value > 0)
                prev.column--;
            auto current = prev;

            while (current.column.value > 0
                   && (!terminal.currentScreen().isCellEmpty(current)
                       || terminal.currentScreen().isCellEmpty(prev)))
            {
                prev.column = current.column;
                current.column--;
            }
            if (current.column.value == 0)
                return current;
            else
                return prev;
        }
        case ViMotion::WordEndForward: { // e
            auto const rightMargin = terminal.pageSize().columns.as<ColumnOffset>();
            auto prev = cursorPosition;
            if (prev.column + 1 < rightMargin)
                prev.column++;
            auto current = prev;
            while (current.column + 1 < rightMargin
                   && (!terminal.currentScreen().isCellEmpty(current)
                       || terminal.currentScreen().isCellEmpty(prev)))
            {
                prev.column = current.column;
                current.column++;
            }
            return prev;
        }
        case ViMotion::WordForward: { // w
            auto const rightMargin = terminal.pageSize().columns.as<ColumnOffset>();
            auto prev = cursorPosition;
            if (prev.column + 1 < rightMargin)
                prev.column++;
            auto current = prev;
            while (current.column + 1 < rightMargin
                   && (terminal.currentScreen().isCellEmpty(current)
                       || !terminal.currentScreen().isCellEmpty(prev)))
            {
                prev = current;
                current.column++;
            }
            return current;
        }
        case ViMotion::Explicit:  // <special for explicit operations>
        case ViMotion::Selection: // <special for visual modes>
        case ViMotion::FullLine:  // <special for full-line operations>
            return snapToCell(cursorPosition);
    }
    crispy::unreachable();
}

void ViCommands::moveCursor(ViMotion motion, unsigned count)
{
    Require(terminal.inputHandler().mode() != ViMode::Insert);

    auto const nextPosition = translateToCellLocation(motion, count);
    InputLog()("Move cursor: {} to {}\n", motion, nextPosition);
    moveCursorTo(nextPosition);
}

void ViCommands::moveCursorTo(CellLocation position)
{
    cursorPosition = position;

    terminal.viewport().makeVisible(cursorPosition.line);

    switch (terminal.inputHandler().mode())
    {
        case ViMode::Normal:
        case ViMode::Insert: break;
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock:
            if (terminal.selector())
                terminal.selector()->extend(cursorPosition);
            break;
    }

    terminal.screenUpdated();
}

} // namespace terminal
