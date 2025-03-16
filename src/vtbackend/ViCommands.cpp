// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Terminal.h>
#include <vtbackend/ViCommands.h>
#include <vtbackend/ViInputHandler.h>
#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <libunicode/ucd.h>

#include <format>
#include <memory>

namespace vtbackend
{

namespace
{
    constexpr bool isWord(char32_t codepoint) noexcept
    {
        // A word consists of a sequence of letters, digits and underscores, or a
        // sequence of other non-blank characters, separated with white space (spaces,
        // tabs, <EOL>).  This can be changed with the 'iskeyword' option.  An empty line
        // is also considered to be a word.
        return ('a' <= codepoint && codepoint <= 'z') || ('A' <= codepoint && codepoint <= 'Z')
               || ('0' <= codepoint && codepoint <= '9') || codepoint == '_';
    }

    constexpr bool isKeyword(char32_t codepoint) noexcept
    {
        // vim default: (default: @,48-57,_,192-255)
        //
        // For '@' characters above 255 check the "word" character class
        // (any character that is not white space or punctuation).
        //
        // TODO: The punctuation test is highly inefficient. Adapt libunicode to allow O(1) access to these.
        return (codepoint > 255
                && !(unicode::general_category::space_separator(codepoint)
                     || unicode::general_category::initial_punctuation(codepoint)
                     || unicode::general_category::final_punctuation(codepoint)
                     || unicode::general_category::open_punctuation(codepoint)
                     || unicode::general_category::close_punctuation(codepoint)
                     || unicode::general_category::dash_punctuation(codepoint)))
               || (192 <= codepoint && codepoint <= 255);
    }

    enum class WordSkipClass : uint8_t
    {
        Word,
        Keyword,
        Whitespace,
        Other
    };

    [[maybe_unused]] std::string_view str(WordSkipClass value)
    {
        switch (value)
        {
            case WordSkipClass::Word: return "Word";
            case WordSkipClass::Keyword: return "Keyword";
            case WordSkipClass::Whitespace: return "Whitespace";
            case WordSkipClass::Other: return "Other";
        }
        return "Wow";
    }

    constexpr WordSkipClass wordSkipClass(char32_t codepoint) noexcept
    {
        if (isWord(codepoint))
            return WordSkipClass::Word;
        else if (isKeyword(codepoint))
            return WordSkipClass::Keyword;
        else if (codepoint == ' ' || codepoint == '\t' || codepoint == 0)
            return WordSkipClass::Whitespace;
        else
            return WordSkipClass::Other;
    }

    WordSkipClass wordSkipClass(std::string text) noexcept
    {
        auto const s32 = unicode::convert_to<char32_t>(std::string_view(text.data(), text.size()));
        switch (s32.size())
        {
            case 0: return WordSkipClass::Whitespace;
            case 1: return wordSkipClass(s32[0]);
            default: return WordSkipClass::Other;
        }
    }

    // constexpr bool shouldSkipForUntilWordBeginReverse(WordSkipClass current, WordSkipClass& initial)
    // noexcept
    // {
    //     auto const result = current == initial
    //                         || (current == WordSkipClass::Whitespace && initial !=
    //                         WordSkipClass::Whitespace);
    //     return result;
    // }

    constexpr bool shouldSkipForUntilWordBegin(WordSkipClass current, WordSkipClass& initial) noexcept
    {
        bool const result = current == initial
                            || (current == WordSkipClass::Whitespace && initial != WordSkipClass::Whitespace);

        if (current == WordSkipClass::Whitespace && initial != WordSkipClass::Whitespace)
            initial = WordSkipClass::Whitespace;

        return result;
    }

    CellLocation getRightMostNonEmptyCellLocation(Terminal const& terminal, LineOffset lineOffset) noexcept
    {
        if (terminal.isPrimaryScreen())
            return terminal.primaryScreen().grid().rightMostNonEmptyAt(lineOffset);
        else
            return terminal.alternateScreen().grid().rightMostNonEmptyAt(lineOffset);
    }

    constexpr std::optional<std::pair<char, bool>> matchingPairOfChar(char32_t input) noexcept
    {
        auto constexpr Pairs = std::array {
            std::pair { U'(', U')' },
            std::pair { U'[', U']' },
            std::pair { U'{', U'}' },
            std::pair { U'<', U'>' },
        };

        for (auto const& pair: Pairs)
        {
            if (input == pair.first)
                return { { pair.second, true } };
            if (input == pair.second)
                return { { pair.first, false } };
        }

        return std::nullopt;
    }

    constexpr bool isValidCharMove(std::optional<ViMotion> motion) noexcept
    {
        if (!motion.has_value())
            return false;
        switch (*motion)
        {
            case ViMotion::TillBeforeCharRight:
            case ViMotion::TillAfterCharLeft:
            case ViMotion::ToCharRight:
            case ViMotion::ToCharLeft: return true;
            default: return false;
        }
    }

    constexpr ViMotion invertCharMove(ViMotion motion) noexcept
    {
        switch (motion)
        {
            case ViMotion::TillBeforeCharRight: return ViMotion::TillAfterCharLeft;
            case ViMotion::TillAfterCharLeft: return ViMotion::TillBeforeCharRight;
            case ViMotion::ToCharRight: return ViMotion::ToCharLeft;
            case ViMotion::ToCharLeft: return ViMotion::ToCharRight;
            default: return motion;
        }
    }

} // namespace

using namespace std;

ViCommands::ViCommands(Terminal& theTerminal): _terminal { &theTerminal }
{
}

void ViCommands::scrollViewport(ScrollOffset delta)
{
    if (delta.value < 0)
        _terminal->viewport().scrollDown(boxed_cast<LineCount>(-delta));
    else
        _terminal->viewport().scrollUp(boxed_cast<LineCount>(delta));
}

void ViCommands::searchStart()
{
    _terminal->screenUpdated();
}

void ViCommands::searchDone()
{
    _terminal->screenUpdated();
}

void ViCommands::searchCancel()
{
    _terminal->search().pattern.clear();
    _terminal->screenUpdated();
}

void ViCommands::promptStart(std::string const& query)
{
    _terminal->setPrompt(query);
    _terminal->screenUpdated();
}

void ViCommands::promptDone()
{
    _terminal->screenUpdated();
}

void ViCommands::promptCancel()
{
    _terminal->screenUpdated();
}

void ViCommands::updatePromptText(std::string const& text)
{
    _terminal->setPromptText(text);
    _terminal->screenUpdated();
}

bool ViCommands::jumpToNextMatch(unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (auto const nextPosition = _terminal->searchNextMatch(cursorPosition))
        {
            inputLog()("jumpToNextMatch");
            _jumpHistory.add(nextPosition.value());
            moveCursorTo(nextPosition.value());
        }
        else
            return false;

    return true;
}

bool ViCommands::jumpToPreviousMatch(unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (auto const nextPosition = _terminal->searchPrevMatch(cursorPosition))
        {
            inputLog()("jumpToPreviousMatch");
            _jumpHistory.add(nextPosition.value());
            moveCursorTo(nextPosition.value());
        }
        else
            return false;

    return true;
}

void ViCommands::updateSearchTerm(std::u32string const& text)
{
    if (auto const newLocation = _terminal->searchReverse(text, cursorPosition))
        moveCursorTo(newLocation.value());
}

void ViCommands::modeChanged(ViMode mode)
{
    auto _ = crispy::finally { [this, mode]() {
        _lastMode = mode;
    } };

    inputLog()("mode changed to {}\n", mode);

    auto const selectFrom = _terminal->selector() ? _terminal->selector()->from() : cursorPosition;

    switch (mode)
    {
        case ViMode::Insert:
            // Force re-render as viewport & cursor might have changed.
            _terminal->setMode(DECMode::VisibleCursor, _lastCursorVisible);
            _terminal->setCursorShape(_lastCursorShape);
            _terminal->viewport().forceScrollToBottom();
            _terminal->clearSearch();
            _terminal->popStatusDisplay();
            break;
        case ViMode::Normal:
            _lastCursorShape = _terminal->cursorShape();
            _lastCursorVisible = _terminal->isModeEnabled(DECMode::VisibleCursor);
            _terminal->setMode(DECMode::VisibleCursor, true);

            if (_lastMode == ViMode::Insert)
                cursorPosition = _terminal->currentScreen().cursor().position;
            if (_terminal->selectionAvailable())
                _terminal->clearSelection();
            _terminal->pushStatusDisplay(StatusDisplayType::Indicator);
            break;
        case ViMode::Visual:
            _terminal->setSelector(make_unique<LinearSelection>(
                _terminal->selectionHelper(), selectFrom, _terminal->selectionUpdatedHelper()));
            (void) _terminal->selector()->extend(cursorPosition);
            _terminal->pushStatusDisplay(StatusDisplayType::Indicator);
            break;
        case ViMode::VisualLine:
            _terminal->setSelector(make_unique<FullLineSelection>(
                _terminal->selectionHelper(), selectFrom, _terminal->selectionUpdatedHelper()));
            (void) _terminal->selector()->extend(cursorPosition);
            _terminal->pushStatusDisplay(StatusDisplayType::Indicator);
            break;
        case ViMode::VisualBlock:
            _terminal->setSelector(make_unique<RectangularSelection>(
                _terminal->selectionHelper(), selectFrom, _terminal->selectionUpdatedHelper()));
            (void) _terminal->selector()->extend(cursorPosition);
            _terminal->pushStatusDisplay(StatusDisplayType::Indicator);
            break;
    }

    _terminal->screenUpdated();
    _terminal->inputModeChanged(mode);
}

void ViCommands::reverseSearchCurrentWord()
{
    // auto const oldPos = cursorPosition;
    auto const [wordUnderCursor, range] = _terminal->extractWordUnderCursor(cursorPosition);
    assert(range.contains(cursorPosition));
    cursorPosition = range.first;

    updateSearchTerm(wordUnderCursor);
    jumpToPreviousMatch(1);
}

void ViCommands::toggleLineMark()
{
    auto const currentLineFlags = _terminal->currentScreen().lineFlagsAt(cursorPosition.line);
    _terminal->currentScreen().enableLineFlags(
        cursorPosition.line, LineFlag::Marked, !(currentLineFlags & LineFlag::Marked));
}

void ViCommands::searchCurrentWord()
{
    auto const [wordUnderCursor, range] = _terminal->extractWordUnderCursor(cursorPosition);
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
            assert(_terminal->selector());
            if (_lastMode == ViMode::VisualBlock)
                _terminal->setHighlightRange(RectangularHighlight { .from = _terminal->selector()->from(),
                                                                    .to = _terminal->selector()->to() });
            else
                _terminal->setHighlightRange(LinearHighlight { .from = _terminal->selector()->from(),
                                                               .to = _terminal->selector()->to() });
            _terminal->copyToClipboard(_terminal->extractSelectionText());
            _terminal->inputHandler().setMode(ViMode::Normal);
            break;
        }
        default: {
            auto const [from, to] = translateToCellRange(motion, count);
            // motion is inclusive but for yank we want to exclude the last cell which is the first cell of
            // the next word
            executeYank(from, { .line = to.line, .column = to.column - 1 });
        }
        break;
    }
}

std::string ViCommands::extractTextAndHighlightRange(CellLocation from, CellLocation to)
{
    assert(_terminal->inputHandler().mode() == ViMode::Normal);
    assert(!_terminal->selector());

    // TODO: ideally keep that selection for about N msecs,
    // such that it'll be visually rendered and the user has a feedback of what's
    // being clipboarded.
    // Maybe via a event API to inform that a non-visual selection
    // has happened and that it can now either be instantly destroyed
    // or delayed (N msecs, configurable),
    _terminal->setSelector(make_unique<LinearSelection>(
        _terminal->selectionHelper(), from, _terminal->selectionUpdatedHelper()));
    (void) _terminal->selector()->extend(to);

    auto text = _terminal->extractSelectionText();

    _terminal->clearSelection();
    _terminal->setHighlightRange(LinearHighlight { .from = from, .to = to });
    _terminal->inputHandler().setMode(ViMode::Normal);
    _terminal->screenUpdated();
    return text;
}

void ViCommands::executeYank(CellLocation from, CellLocation to)
{
    _terminal->copyToClipboard(extractTextAndHighlightRange(from, to));
}

void ViCommands::executeOpen(CellLocation from, CellLocation to)
{
    _terminal->openDocument(extractTextAndHighlightRange(from, to));
}

void ViCommands::executeOpen(ViMotion motion, unsigned count)
{
    switch (motion)
    {
        case ViMotion::Selection: {
            assert(_terminal->selector());
            if (_lastMode == ViMode::VisualBlock)
                _terminal->setHighlightRange(RectangularHighlight { .from = _terminal->selector()->from(),
                                                                    .to = _terminal->selector()->to() });
            else
                _terminal->setHighlightRange(LinearHighlight { .from = _terminal->selector()->from(),
                                                               .to = _terminal->selector()->to() });

            _terminal->copyToClipboard(_terminal->extractSelectionText());

            _terminal->inputHandler().setMode(ViMode::Normal);
            break;
        }
        default: {
            auto const [from, to] = translateToCellRange(motion, count);
            executeOpen(from, to);
        }
        break;
    }
}

void ViCommands::execute(ViOperator op, ViMotion motion, unsigned count, char32_t lastChar)
{
    inputLog()("{}: Executing: {} {} {}\n", _terminal->inputHandler().mode(), count, op, motion);
    switch (op)
    {
        case ViOperator::MoveCursor:
            //.
            moveCursor(motion, count);
            break;
        case ViOperator::Open:
            if (isValidCharMove(motion))
            {
                _lastCharMotion = motion;
                _lastChar = lastChar;
            }
            executeOpen(motion, count);
            break;
        case ViOperator::Yank:
            if (isValidCharMove(motion))
            {
                _lastCharMotion = motion;
                _lastChar = lastChar;
            }
            executeYank(motion, count);
            if (_terminal->settings().isInsertAfterYank)
                _terminal->inputHandler().setMode(ViMode::Insert);
            break;
        case ViOperator::Paste:
            //.
            _terminal->sendPasteFromClipboard(count, false);
            break;
        case ViOperator::PasteStripped:
            //.
            _terminal->sendPasteFromClipboard(count, true);
            break;
        case ViOperator::ReverseSearchCurrentWord: // TODO: Does this even make sense to have?
            break;
    }
    _terminal->screenUpdated();
}

void ViCommands::select(TextObjectScope scope, TextObject textObject)
{
    auto const [from, to] = translateToCellRange(scope, textObject);
    cursorPosition = to;
    inputLog()("{}: Executing: select {} {} [{} .. {}]\n",
               _terminal->inputHandler().mode(),
               scope,
               textObject,
               from,
               to);
    _terminal->setSelector(make_unique<LinearSelection>(
        _terminal->selectionHelper(), from, _terminal->selectionUpdatedHelper()));
    (void) _terminal->selector()->extend(to);
    _terminal->screenUpdated();
}

void ViCommands::yank(TextObjectScope scope, TextObject textObject)
{
    auto const [from, to] = translateToCellRange(scope, textObject);
    cursorPosition = from;
    inputLog()("{}: Executing: yank {} {}\n", _terminal->inputHandler().mode(), scope, textObject);
    executeYank(from, to);
    _terminal->screenUpdated();
}

void ViCommands::yank(ViMotion motion)
{
    auto const [from, to] = translateToCellRange(motion, 1);
    cursorPosition = from;
    inputLog()("{}: Executing: motion-yank {}\n", _terminal->inputHandler().mode(), motion);
    executeYank(from, to);
    _terminal->screenUpdated();
}

void ViCommands::open(TextObjectScope scope, TextObject textObject)
{
    auto const [from, to] = translateToCellRange(scope, textObject);
    cursorPosition = from;
    inputLog()("{}: Executing: open {} {}\n", _terminal->inputHandler().mode(), scope, textObject);
    executeOpen(from, to);
    _terminal->screenUpdated();
}

void ViCommands::paste(unsigned count, bool stripped)
{
    _terminal->sendPasteFromClipboard(count, stripped);
}

CellLocation ViCommands::prev(CellLocation location) const noexcept
{
    if (location.column.value > 0)
        return { .line = location.line, .column = location.column - 1 };

    auto const topLineOffset = _terminal->isPrimaryScreen()
                                   ? -boxed_cast<LineOffset>(_terminal->primaryScreen().historyLineCount())
                                   : LineOffset(0);
    if (location.line > topLineOffset)
    {
        location = getRightMostNonEmptyCellLocation(*_terminal, location.line - 1);
        if (location.column + 1 < boxed_cast<ColumnOffset>(_terminal->pageSize().columns))
            ++location.column;
    }

    return location;
}

CellLocation ViCommands::next(CellLocation location) const noexcept
{
    auto const rightMargin = _terminal->pageSize().columns.as<ColumnOffset>() - 1;
    if (location.column < rightMargin)
    {
        auto const width = max(uint8_t { 1 }, _terminal->currentScreen().cellWidthAt(location));
        return { .line = location.line, .column = location.column + ColumnOffset::cast_from(width) };
    }

    if (location.line < boxed_cast<LineOffset>(_terminal->pageSize().lines - 1))
    {
        location.line++;
        location.column = ColumnOffset(0);
    }

    return location;
}

CellLocation ViCommands::findMatchingPairFrom(CellLocation location) const noexcept
{
    auto const& cell = _terminal->primaryScreen().at(cursorPosition);
    if (cell.codepointCount() != 1)
        return location;

    auto const a = cell.codepoint(0);
    auto const matchResult = matchingPairOfChar(a);
    if (!matchResult)
        return location;
    auto const [b, left] = *matchResult;

    if (left)
        return findMatchingPairRight(a, b, 0);
    else
        return findMatchingPairLeft(b, a, 0);
}

CellLocation ViCommands::findMatchingPairLeft(char32_t left, char32_t right, int initialDepth) const noexcept
{
    auto a = cursorPosition;
    auto depth = initialDepth;

    while (true)
    {
        if (compareCellTextAt(a, right))
        {
            ++depth;
            if (depth == 0)
                break;
        }
        else if (compareCellTextAt(a, left))
        {
            --depth;
            if (depth == 0)
                break;
        }

        if (auto const prevA = prev(a); prevA != a)
            a = prevA;
        else
            break;
    }
    return a;
}

CellLocation ViCommands::findMatchingPairRight(char32_t left, char32_t right, int initialDepth) const noexcept
{
    auto depth = initialDepth;
    auto b = cursorPosition;

    while (true)
    {
        if (compareCellTextAt(b, left))
        {
            ++depth;
            if (depth == 0)
                break;
        }
        else if (compareCellTextAt(b, right))
        {
            --depth;
            if (depth == 0)
                break;
        }

        if (auto const nextB = next(b); nextB != b)
            b = nextB;
        else
            break;
    }

    return b;
}

CellLocationRange ViCommands::expandMatchingPair(TextObjectScope scope, char left, char right) const noexcept
{
    auto a = findMatchingPairLeft(left, right, left != right ? 1 : -1);
    auto b = findMatchingPairRight(left, right, left != right ? 1 : -1);

    if (scope == TextObjectScope::Inner)
    {
        if (compareCellTextAt(a, left))
            a = next(a);
        if (compareCellTextAt(b, right))
            b = prev(b);
    }

    return { .first = a, .second = b };
}

CellLocationRange ViCommands::translateToCellRange(TextObjectScope scope,
                                                   TextObject textObject) const noexcept
{
    auto const gridTop = -_terminal->currentScreen().historyLineCount().as<LineOffset>();
    auto const gridBottom = _terminal->pageSize().lines.as<LineOffset>() - 1;
    auto const rightMargin = _terminal->pageSize().columns.as<ColumnOffset>() - 1;
    auto a = cursorPosition;
    auto b = cursorPosition;
    switch (textObject)
    {
        case TextObject::AngleBrackets: return expandMatchingPair(scope, '<', '>');
        case TextObject::BackQuotes: return expandMatchingPair(scope, '`', '`');
        case TextObject::CurlyBrackets: return expandMatchingPair(scope, '{', '}');
        case TextObject::DoubleQuotes: return expandMatchingPair(scope, '"', '"');
        case TextObject::LineMark:
            // Walk the line upwards until we find a marked line.
            while (a.line > gridTop
                   && !(_terminal->currentScreen().lineFlagsAt(a.line).contains(LineFlag::Marked)))
                --a.line;
            if (scope == TextObjectScope::Inner && a != cursorPosition)
                ++a.line;
            // Walk the line downwards until we find a marked line.
            while (b.line < gridBottom
                   && !(_terminal->currentScreen().lineFlagsAt(b.line).contains(LineFlag::Marked)))
                ++b.line;
            if (scope == TextObjectScope::Inner && b != cursorPosition)
                --b.line;
            // Span the range from left most column to right most column.
            a.column = ColumnOffset(0);
            b.column = rightMargin;
            break;
        case TextObject::Paragraph:
            while (a.line > gridTop && !_terminal->currentScreen().isLineEmpty(a.line - 1))
                --a.line;
            while (b.line < gridBottom && !_terminal->currentScreen().isLineEmpty(b.line))
                ++b.line;
            break;
        case TextObject::RoundBrackets: return expandMatchingPair(scope, '(', ')');
        case TextObject::SingleQuotes: return expandMatchingPair(scope, '\'', '\'');
        case TextObject::SquareBrackets: return expandMatchingPair(scope, '[', ']');
        case TextObject::Word: {
            a = findBeginOfWordAt(a, JumpOver::No);
            b = findEndOfWordAt(b, JumpOver::No);
            break;
        }
        case TextObject::BigWord: {
            while (a.column.value > 0 && !_terminal->currentScreen().isCellEmpty(prev(a)))
                a = prev(a);
            while (b.column < rightMargin && !_terminal->currentScreen().isCellEmpty(next(b)))
                b = next(b);
            break;
        }
    }
    return { .first = a, .second = b };
}

CellLocationRange ViCommands::translateToCellRange(ViMotion motion, unsigned count) noexcept
{
    switch (motion)
    {
        case ViMotion::FullLine:
            return { .first = cursorPosition - cursorPosition.column,
                     .second = { .line = cursorPosition.line,
                                 .column = _terminal->pageSize().columns.as<ColumnOffset>() - 1 } };
        default:
            //.
            return { .first = cursorPosition, .second = translateToCellLocationAndRecord(motion, count) };
    }
}

CellLocation ViCommands::findBeginOfWordAt(CellLocation location, JumpOver jumpOver) const noexcept
{
    auto const firstAddressableLocation =
        CellLocation { .line = -LineOffset::cast_from(_terminal->currentScreen().historyLineCount()),
                       .column = ColumnOffset(0) };

    auto current = location;
    auto leftLocation = prev(current);
    auto leftClass = wordSkipClass(_terminal->currentScreen().cellTextAt(leftLocation));
    auto continuationClass =
        jumpOver == JumpOver::Yes ? leftClass : wordSkipClass(_terminal->currentScreen().cellTextAt(current));

    while (current != firstAddressableLocation && leftClass == continuationClass)
    {
        current = leftLocation;
        leftLocation = prev(current);
        leftClass = wordSkipClass(_terminal->currentScreen().cellTextAt(leftLocation));
        if (continuationClass == WordSkipClass::Whitespace && leftClass != WordSkipClass::Whitespace)
            continuationClass = leftClass;
    }

    return current;
}

CellLocation ViCommands::findEndOfWordAt(CellLocation location, JumpOver jumpOver) const noexcept
{
    auto const rightMargin = _terminal->pageSize().columns.as<ColumnOffset>();
    auto leftOfCurrent = location;
    if (leftOfCurrent.column + 1 < rightMargin && jumpOver == JumpOver::Yes)
        leftOfCurrent.column++;
    auto current = leftOfCurrent;
    while (current.column + 1 < rightMargin
           && !(!_terminal->wordDelimited(leftOfCurrent) && _terminal->wordDelimited(current)))
    {
        leftOfCurrent.column = current.column;
        current.column++;
    }
    return leftOfCurrent;
}

CellLocation ViCommands::snapToCell(CellLocation location) const noexcept
{
    while (location.column > ColumnOffset(0) && compareCellTextAt(location, '\0'))
        --location.column;

    return location;
}

CellLocation ViCommands::snapToCellRight(CellLocation location) const noexcept
{
    auto const rightMargin = ColumnOffset::cast_from(_terminal->pageSize().columns - 1);
    while (location.column < rightMargin && compareCellTextAt(location, '\0'))
        ++location.column;
    return location;
}

bool ViCommands::compareCellTextAt(CellLocation position, char32_t codepoint) const noexcept
{
    return _terminal->currentScreen().compareCellTextAt(position, codepoint);
}

CellLocation ViCommands::globalCharUp(CellLocation location, char ch, unsigned count) const noexcept
{
    auto const pageTop = -_terminal->currentScreen().historyLineCount().as<LineOffset>();
    auto result = CellLocation { .line = location.line, .column = ColumnOffset(0) };
    while (count > 0)
    {
        if (location.column == ColumnOffset(0) && result.line > pageTop)
            --result.line;
        while (result.line > pageTop)
        {
            auto const& line = _terminal->currentScreen().lineTextAt(result.line, false, true);
            if (line.size() == 1 && line[0] == ch)
                break;
            --result.line;
        }
        --count;
    }
    return result;
}

CellLocation ViCommands::globalCharDown(CellLocation location, char ch, unsigned count) const noexcept
{
    auto const pageBottom = _terminal->pageSize().lines.as<LineOffset>() - 1;
    auto result = CellLocation { .line = location.line, .column = ColumnOffset(0) };
    while (count > 0)
    {
        if (location.column == ColumnOffset(0) && result.line < pageBottom)
            ++result.line;
        while (result.line < pageBottom)
        {
            auto const& line = _terminal->currentScreen().lineTextAt(result.line, false, true);
            if (line.size() == 1 && line[0] == ch)
                break;
            ++result.line;
        }
        --count;
    }
    return result;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
CellLocation ViCommands::translateToCellLocationAndRecord(ViMotion motion, unsigned count) noexcept
{
    auto addJumpHistory = [this](CellLocation const& location) {
        inputLog()("addJumpHistory: {}:{}", location.line, location.column);
        _jumpHistory.add(location);
        return location;
    };
    switch (motion)
    {
        case ViMotion::CharLeft: // h
        {
            auto resultPosition = cursorPosition;
            while (count)
            {
                resultPosition = prev(resultPosition);
                --count;
            }
            return resultPosition;
        }
        case ViMotion::CharRight: // l
        {
            auto resultPosition = cursorPosition;
            while (count)
            {
                resultPosition = next(resultPosition);
                --count;
            }
            return resultPosition;
        }
        case ViMotion::ScreenColumn: // |
            return snapToCell({ .line = cursorPosition.line,
                                .column = min(ColumnOffset::cast_from(count - 1),
                                              _terminal->pageSize().columns.as<ColumnOffset>() - 1) });
        case ViMotion::FileBegin: // gg
            return snapToCell(
                { .line = LineOffset::cast_from(-_terminal->currentScreen().historyLineCount().as<int>()),
                  .column = ColumnOffset(0) });
        case ViMotion::FileEnd: // G
            return addJumpHistory(snapToCell(
                { .line = _terminal->pageSize().lines.as<LineOffset>() - 1, .column = ColumnOffset(0) }));
        case ViMotion::PageTop: // <S-H>
            return snapToCell({ .line = boxed_cast<LineOffset>(-_terminal->viewport().scrollOffset())
                                        + *_terminal->viewport().scrollOff(),
                                .column = ColumnOffset(0) });
        case ViMotion::PageBottom: // <S-L>
            return snapToCell({ .line = boxed_cast<LineOffset>(-_terminal->viewport().scrollOffset())
                                        + boxed_cast<LineOffset>(_terminal->pageSize().lines
                                                                 - *_terminal->viewport().scrollOff() - 1),
                                .column = ColumnOffset(0) });
        case ViMotion::LineBegin: // 0
            return { .line = cursorPosition.line, .column = ColumnOffset(0) };
        case ViMotion::LineTextBegin: // ^
        {
            auto result = CellLocation { .line = cursorPosition.line, .column = ColumnOffset(0) };
            while (result.column < _terminal->pageSize().columns.as<ColumnOffset>() - 1
                   && _terminal->currentScreen().isCellEmpty(result))
                ++result.column;
            return result;
        }
        case ViMotion::LineDown: // j
            return { .line = min(cursorPosition.line + LineOffset::cast_from(count),
                                 _terminal->pageSize().lines.as<LineOffset>() - 1),
                     .column = cursorPosition.column };
        case ViMotion::LineEnd: // $
            return getRightMostNonEmptyCellLocation(*_terminal, cursorPosition.line);
        case ViMotion::LineUp: // k
            return { .line = max(cursorPosition.line - LineOffset::cast_from(count),
                                 -_terminal->currentScreen().historyLineCount().as<LineOffset>()),
                     .column = cursorPosition.column };
        case ViMotion::LinesCenter: // M
            return addJumpHistory({ .line = LineOffset::cast_from(_terminal->pageSize().lines / 2 - 1)
                                            - boxed_cast<LineOffset>(_terminal->viewport().scrollOffset()),
                                    .column = cursorPosition.column });
        case ViMotion::PageDown:
            return { .line = min(cursorPosition.line + LineOffset::cast_from(_terminal->pageSize().lines / 2),
                                 _terminal->pageSize().lines.as<LineOffset>() - 1),
                     .column = cursorPosition.column };
        case ViMotion::PageUp:
            return { .line = max(cursorPosition.line - LineOffset::cast_from(_terminal->pageSize().lines / 2),
                                 -_terminal->currentScreen().historyLineCount().as<LineOffset>()),
                     .column = cursorPosition.column };
            return cursorPosition
                   - min(cursorPosition.line, LineOffset::cast_from(_terminal->pageSize().lines) / 2);
        case ViMotion::ParagraphBackward: // {
        {
            auto const pageTop = -_terminal->currentScreen().historyLineCount().as<LineOffset>();
            auto prev = CellLocation { .line = cursorPosition.line, .column = ColumnOffset(0) };
            if (prev.line.value > 0)
                prev.line--;
            auto current = prev;
            while (current.line > pageTop
                   && (!_terminal->currentScreen().isLineEmpty(current.line)
                       || _terminal->currentScreen().isLineEmpty(prev.line)))
            {
                prev.line = current.line;
                current.line--;
            }
            return addJumpHistory(snapToCell(current));
        }
        case ViMotion::GlobalCurlyOpenUp: // [[
            return addJumpHistory(globalCharUp(cursorPosition, '{', count));
        case ViMotion::GlobalCurlyOpenDown: // ]]
            return addJumpHistory(globalCharDown(cursorPosition, '{', count));
        case ViMotion::GlobalCurlyCloseUp: // []
            return addJumpHistory(globalCharUp(cursorPosition, '}', count));
        case ViMotion::GlobalCurlyCloseDown: // ][
            return addJumpHistory(globalCharDown(cursorPosition, '}', count));
        case ViMotion::LineMarkUp: // [m
        {
            auto const gridTop = -_terminal->currentScreen().historyLineCount().as<LineOffset>();
            auto result = CellLocation { .line = cursorPosition.line, .column = ColumnOffset(0) };
            while (count > 0)
            {
                if (result.line > gridTop
                    && _terminal->currentScreen().isLineFlagEnabledAt(result.line, LineFlag::Marked))
                    --result.line;
                while (result.line > gridTop
                       && !_terminal->currentScreen().isLineFlagEnabledAt(result.line, LineFlag::Marked))
                    --result.line;
                --count;
            }
            return addJumpHistory(result);
        }
        case ViMotion::LineMarkDown: // ]m
        {
            auto const pageBottom = _terminal->pageSize().lines.as<LineOffset>() - 1;
            auto result = CellLocation { .line = cursorPosition.line, .column = ColumnOffset(0) };
            while (count > 0)
            {
                if (cursorPosition.column == ColumnOffset(0) && result.line < pageBottom)
                    ++result.line;
                while (result.line < pageBottom)
                {
                    if (_terminal->currentScreen().lineFlagsAt(result.line).contains(LineFlag::Marked))
                        break;
                    ++result.line;
                }
                --count;
            }
            return addJumpHistory(result);
        }
        case ViMotion::ParagraphForward: // }
        {
            auto const pageBottom = _terminal->pageSize().lines.as<LineOffset>() - 1;
            auto prev = CellLocation { .line = cursorPosition.line, .column = ColumnOffset(0) };
            if (prev.line < pageBottom)
                prev.line++;
            auto current = prev;
            while (current.line < pageBottom
                   && (!_terminal->currentScreen().isLineEmpty(current.line)
                       || _terminal->currentScreen().isLineEmpty(prev.line)))
            {
                prev.line = current.line;
                current.line++;
            }
            return addJumpHistory(snapToCell(current));
        }
        case ViMotion::ParenthesisMatching: // % TODO
            return findMatchingPairFrom(cursorPosition);
        case ViMotion::SearchResultBackward: // N TODO
        {
            auto startPosition = cursorPosition;
            for (unsigned i = 0; i < count; ++i)
            {
                startPosition = prev(startPosition);
                auto const nextPosition = _terminal->searchReverse(startPosition);
                if (!nextPosition)
                    return cursorPosition;

                startPosition = *nextPosition;
            }
            return addJumpHistory(startPosition);
        }
        case ViMotion::SearchResultForward: // n
        {
            auto startPosition = cursorPosition;
            for (unsigned i = 0; i < count; ++i)
            {
                startPosition = next(startPosition);
                auto const nextPosition = _terminal->search(startPosition);
                if (!nextPosition)
                    return cursorPosition;
                startPosition = *nextPosition;
            }
            return addJumpHistory(startPosition);
        }
        case ViMotion::WordBackward: // b
        {
            auto current = cursorPosition;
            for (unsigned i = 0; i < count; ++i)
                current = findBeginOfWordAt(current, JumpOver::Yes);

            return current;
        }
        case ViMotion::WordEndForward: // e
        {
            auto current = cursorPosition;
            for (unsigned i = 0; i < count; ++i)
                current = findEndOfWordAt(cursorPosition, JumpOver::Yes);
            return current;
        }
        case ViMotion::BigWordForward: // W
        {
            auto const rightMargin = _terminal->pageSize().columns.as<ColumnOffset>();
            auto prev = cursorPosition;
            if (prev.column + 1 < rightMargin)
                prev.column++;
            auto current = prev;
            while (current.column + 1 < rightMargin
                   && (_terminal->currentScreen().isCellEmpty(current)
                       || !_terminal->currentScreen().isCellEmpty(prev)))
            {
                prev = current;
                current.column++;
            }

            return current;
        }
        case ViMotion::BigWordEndForward: // E
        {
            auto const rightMargin = _terminal->pageSize().columns.as<ColumnOffset>();
            auto prev = cursorPosition;
            if (prev.column + 1 < rightMargin)
                prev.column++;
            auto current = prev;
            while (current.column + 1 < rightMargin
                   && (!_terminal->currentScreen().isCellEmpty(current)
                       || _terminal->currentScreen().isCellEmpty(prev)))
            {
                prev.column = current.column;
                current.column++;
            }
            return prev;
        }
        case ViMotion::BigWordBackward: // B
        {
            auto prev = cursorPosition;
            if (prev.column.value > 0)
                prev.column--;
            auto current = prev;

            while (current.column.value > 0
                   && (!_terminal->currentScreen().isCellEmpty(current)
                       || _terminal->currentScreen().isCellEmpty(prev)))
            {
                prev.column = current.column;
                current.column--;
            }
            if (current.column.value == 0)
                return current;
            else
                return prev;
        }
        case ViMotion::WordForward: // w
        {
            auto const lastAddressableLocation =
                CellLocation { .line = LineOffset::cast_from(_terminal->pageSize().lines - 1),
                               .column = ColumnOffset::cast_from(_terminal->pageSize().columns - 1) };
            auto result = cursorPosition;
            while (count > 0)
            {
                auto initialClass = wordSkipClass(_terminal->currentScreen().cellTextAt(result));
                result = next(result);
                while (result != lastAddressableLocation
                       && shouldSkipForUntilWordBegin(
                           wordSkipClass(_terminal->currentScreen().cellTextAt(result)), initialClass))
                    result = next(result);
                --count;
            }

            return result;
        }
        case ViMotion::Explicit:  // <special for explicit operations>
        case ViMotion::Selection: // <special for visual modes>
        case ViMotion::FullLine:  // <special for full-line operations>
            return snapToCell(cursorPosition);
        case ViMotion::TillBeforeCharRight: // t {char}
            if (auto const result = toCharRight(count); result)
                return result.value() - ColumnOffset(1);
            else
                return cursorPosition;
        case ViMotion::TillAfterCharLeft: // T {char}
            if (auto const result = toCharLeft(count); result)
                return result.value() + ColumnOffset(1);
            else
                return cursorPosition;
        case ViMotion::ToCharRight: // f {char}
            return toCharRight(count).value_or(cursorPosition);
        case ViMotion::ToCharLeft: // F {char}
            return toCharLeft(count).value_or(cursorPosition);
        case ViMotion::RepeatCharMove:
            if (isValidCharMove(_lastCharMotion))
                return translateToCellLocationAndRecord(*_lastCharMotion, count);
            return cursorPosition;
        case ViMotion::RepeatCharMoveReverse:
            if (isValidCharMove(_lastCharMotion))
                return translateToCellLocationAndRecord(invertCharMove(*_lastCharMotion), count);
            return cursorPosition;
        case ViMotion::JumpToLastJumpPoint: return _jumpHistory.jumpToLast(cursorPosition);
        case ViMotion::JumpToMarkBackward: return _jumpHistory.jumpToMarkBackward(cursorPosition);
        case ViMotion::JumpToMarkForward: return _jumpHistory.jumpToMarkForward(cursorPosition);
        case ViMotion::CenterCursor: {
            _terminal->viewport().makeVisibleWithinSafeArea(unbox<LineOffset>(cursorPosition.line),
                                                            LineCount(_terminal->pageSize().lines / 2));
            return cursorPosition;
        }
    }
    crispy::unreachable();
}

optional<CellLocation> ViCommands::toCharRight(CellLocation startPosition) const noexcept
{
    auto result = next(startPosition);
    auto const rightMargin = _terminal->pageSize().columns.as<ColumnOffset>() - 1;
    while (true)
    {
        // if on wrong line
        if (result.line != startPosition.line)
            return std::nullopt;
        if (_terminal->currentScreen().compareCellTextAt(result, _lastChar))
            return result;
        // if reached end of the line
        if (result.column == rightMargin)
            return std::nullopt;
        result = next(result);
    }
}

optional<CellLocation> ViCommands::toCharLeft(CellLocation startPosition) const noexcept
{
    auto result = prev(startPosition);

    while (true)
    {
        if (result.line != startPosition.line)
            return std::nullopt;
        if (_terminal->currentScreen().compareCellTextAt(result, _lastChar))
            return result;
        result = prev(result);
    }
}

optional<CellLocation> ViCommands::toCharRight(unsigned count) const noexcept
{
    auto result = optional { cursorPosition };
    while (count > 0 && result.has_value())
    {
        result = toCharRight(*result);
        --count;
    }
    return result;
}

optional<CellLocation> ViCommands::toCharLeft(unsigned count) const noexcept
{
    auto result = optional { cursorPosition };
    while (count > 0 && result.has_value())
    {
        result = toCharLeft(*result);
        --count;
    }
    return result;
}

void ViCommands::moveCursor(ViMotion motion, unsigned count, char32_t lastChar)
{
    Require(_terminal->inputHandler().mode() != ViMode::Insert);

    if (isValidCharMove(motion))
    {
        _lastCharMotion = motion;
        _lastChar = lastChar;
    }

    auto const nextPosition = translateToCellLocationAndRecord(motion, count);
    inputLog()("Move cursor: {} to {}\n", motion, nextPosition);
    moveCursorTo(nextPosition);
}

void ViCommands::moveCursorTo(CellLocation position)
{
    cursorPosition = position;

    _terminal->viewport().makeVisibleWithinSafeArea(cursorPosition.line);

    switch (_terminal->inputHandler().mode())
    {
        case ViMode::Normal:
        case ViMode::Insert: break;
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock:
            if (_terminal->selector())
                (void) _terminal->selector()->extend(cursorPosition);
            break;
    }

    _terminal->screenUpdated();
}

} // namespace vtbackend
