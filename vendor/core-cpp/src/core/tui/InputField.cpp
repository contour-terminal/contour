// SPDX-License-Identifier: Apache-2.0
#include <core/tui/InputField.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/EditAction.hpp>
#include <core/tui/Theme.hpp>
#include <core/tui/Unicode.hpp>

#include <libunicode/ucd.h>
#include <libunicode/utf8_grapheme_segmenter.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ranges>
#include <string>
#include <string_view>

namespace core::tui
{

namespace
{
    /// @brief Encodes a Unicode codepoint as UTF-8.
    /// @param cp The codepoint to encode.
    /// @return UTF-8 encoded string.
    auto encodeUtf8(char32_t cp) -> std::string
    {
        auto result = std::string {};
        if (cp < 0x80)
        {
            result += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            result += static_cast<char>(0xC0 | (cp >> 6));
            result += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            result += static_cast<char>(0xE0 | (cp >> 12));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x110000)
        {
            result += static_cast<char>(0xF0 | (cp >> 18));
            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return result;
    }

    /// @brief Advances past one UTF-8 codepoint.
    auto nextUtf8(std::string_view s, std::size_t pos) -> std::size_t
    {
        if (pos >= s.size())
            return pos;
        ++pos;
        while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
            ++pos;
        return pos;
    }

    /// @brief Moves back one UTF-8 codepoint.
    auto prevUtf8(std::string_view s, std::size_t pos) -> std::size_t
    {
        if (pos == 0)
            return 0;
        --pos;
        while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
            --pos;
        return pos;
    }

    /// @brief Decodes the UTF-8 codepoint that starts at the given byte offset.
    /// @param s UTF-8 encoded text.
    /// @param pos Byte offset of a codepoint's lead byte (must be a codepoint boundary).
    /// @return The decoded codepoint, or U+FFFD (replacement) for malformed input.
    auto decodeUtf8At(std::string_view s, std::size_t pos) -> char32_t
    {
        if (pos >= s.size())
            return U'\uFFFD';
        auto const lead = static_cast<unsigned char>(s[pos]);
        if (lead < 0x80)
            return static_cast<char32_t>(lead);

        auto extraBytes = std::size_t { 0 };
        auto cp = char32_t { 0 };
        if ((lead & 0xE0) == 0xC0)
        {
            extraBytes = 1;
            cp = lead & 0x1F;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            extraBytes = 2;
            cp = lead & 0x0F;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            extraBytes = 3;
            cp = lead & 0x07;
        }
        else
        {
            return U'\uFFFD'; // Stray continuation byte or invalid lead byte.
        }

        if (pos + extraBytes >= s.size())
            return U'\uFFFD';
        for (auto const i: std::views::iota(std::size_t { 1 }, extraBytes + 1))
        {
            auto const cont = static_cast<unsigned char>(s[pos + i]);
            if ((cont & 0xC0) != 0x80)
                return U'\uFFFD';
            cp = (cp << 6) | (cont & 0x3F);
        }
        return cp;
    }

    /// @brief Tests whether a codepoint is whitespace for word-boundary purposes.
    /// Covers ASCII whitespace plus Unicode space/line/paragraph separators (e.g. U+00A0 NBSP,
    /// U+2003 EM SPACE, U+3000 IDEOGRAPHIC SPACE), so multibyte separators delimit words too.
    /// @param cp The codepoint to test.
    /// @return True if the codepoint is whitespace.
    auto isUnicodeWhitespace(char32_t cp) -> bool
    {
        if (cp < 0x80)
            return std::isspace(static_cast<unsigned char>(cp)) != 0;
        return unicode::general_category::is_space_separator(cp)
               || unicode::general_category::is_line_separator(cp)
               || unicode::general_category::is_paragraph_separator(cp);
    }

    /// @brief Sanitizes text for single-line display.
    /// Replaces '\n' with space and strips '\r' entirely.
    /// @param text The input text to sanitize.
    /// @return Sanitized string safe for single-line rendering.
    auto sanitizeForSingleLine(std::string_view text) -> std::string
    {
        auto result = std::string {};
        result.reserve(text.size());
        for (auto const ch: text)
        {
            if (ch == '\r')
                continue;
            if (ch == '\n')
                result += ' ';
            else
                result += ch;
        }
        return result;
    }

    /// @brief Truncates text at the first line break for use as single-line ghost text.
    ///
    /// The inline suggestion renders on one line and is committed verbatim on accept, so any
    /// content past the first '\n' or '\r' must be dropped — a bare '\r' would otherwise render
    /// as a stray control glyph and be committed unsanitized (unlike buffer text, which goes
    /// through sanitizeForSingleLine). Centralizes the rule shared by setGhostText and
    /// prependGhostText so they cannot drift.
    /// @param ghost The candidate ghost text.
    /// @return @p ghost truncated at the first '\n' or '\r' (whichever comes first).
    auto firstGhostLine(std::string_view ghost) -> std::string_view
    {
        if (auto const pos = ghost.find_first_of("\r\n"); pos != std::string_view::npos)
            ghost = ghost.substr(0, pos);
        return ghost;
    }
} // namespace

InputField::InputField(): InputField(defaultSteadyClock())
{
}

InputField::InputField(IClock const& clock): _clock(clock)
{
}

auto InputField::processEvent(InputEvent const& event) -> InputFieldAction
{
    if (auto const* key = std::get_if<KeyEvent>(&event))
        return handleKey(*key);

    if (auto const* paste = std::get_if<PasteEvent>(&event))
    {
        // Save undo state before any changes
        saveUndoState();
        clearGhostText(); // User input clears ghost suggestion
        // Delete selection first if any (paste replaces selection)
        if (hasSelection())
            deleteSelection();
        insertText(paste->text);
        _lastWasKill = false;
        return InputFieldAction::Changed;
    }

    return InputFieldAction::None;
}

// --- Component Interface Implementation ---

void InputField::render(Canvas& canvas)
{
    auto const& theme = canvas.theme();
    auto const width = canvas.width();
    auto const height = canvas.height();

    if (width <= 0 || height <= 0)
        return;

    // Determine styles: use custom styles if set, otherwise theme defaults
    Style const textStyle = _styles.text.value_or(focused() ? theme.inputFocused : theme.inputNormal);
    Style selectionStyle = _styles.selection.value_or(textStyle);
    if (!_styles.selection.has_value())
        selectionStyle.inverse = true;
    Style const ghostStyle = _styles.ghost.value_or(theme.ghostText);

    // Fill background if custom background style is set
    if (_styles.background.has_value())
        canvas.fill(Rect { .x = 0, .y = 0, .width = width, .height = height }, ' ', *_styles.background);

    // ====================================================================
    // Single-line mode (or single-line content)
    // ====================================================================
    if (!_multiline || lineCount() <= 1)
    {
        // Fill background with decorator bg (aurora gradient or flat)
        if (_textDecorator)
        {
            for (auto const c: std::views::iota(0, std::max(0, width)))
            {
                Style cellStyle = _styles.background.value_or(Style {});
                if (auto bg = _textDecorator->background(c))
                    cellStyle.bg = *bg;
                canvas.put(0, c, " ", cellStyle);
            }
        }

        int col = 0;

        // Render prompt with per-column decorator background
        if (!_prompt.empty())
        {
            auto promptSeg = unicode::utf8_grapheme_segmenter(_prompt);
            auto pNext = promptSeg.begin();
            while (pNext != promptSeg.end() && col < width)
            {
                auto const pIt = pNext;
                ++pNext;
                char const* pStart = pIt._clusterStart;
                char const* pEnd =
                    (pNext != promptSeg.end()) ? pNext._clusterStart : (_prompt.data() + _prompt.size());
                Style pStyle = textStyle;
                if (_textDecorator)
                {
                    if (auto bg = _textDecorator->background(col))
                        pStyle.bg = *bg;
                }
                auto const pView = std::string_view(pStart, static_cast<std::size_t>(pEnd - pStart));
                col += canvas.putString(0, col, pView, pStyle);
            }
        }

        auto const textStartCol = col;
        auto const availableWidth = width - textStartCol;
        if (availableWidth <= 0)
            return;

        // Measure pass: compute total text width in display columns and the cursor's
        // column within the unscrolled text, so we can adjust the horizontal scroll
        // offset to keep the cursor visible.
        auto segmenter = unicode::utf8_grapheme_segmenter(_buffer);
        int totalTextCols = 0;
        int cursorTextCol = 0;
        bool cursorMeasured = false;
        auto measureIt = segmenter.begin();
        while (measureIt != segmenter.end())
        {
            char const* clusterStart = measureIt._clusterStart;
            auto const clusterByteStart = static_cast<std::size_t>(clusterStart - _buffer.data());
            if (!cursorMeasured && _cursor <= clusterByteStart)
            {
                cursorTextCol = totalTextCols;
                cursorMeasured = true;
            }
            int const clusterWidth = _masked ? 1 : graphemeClusterWidth(*measureIt);
            totalTextCols += clusterWidth;
            ++measureIt;
        }
        if (!cursorMeasured)
            cursorTextCol = totalTextCols;

        // Adjust horizontal scroll offset to keep cursor in view.
        if (cursorTextCol < _hScrollOffset)
            _hScrollOffset = cursorTextCol;
        else if (cursorTextCol >= _hScrollOffset + availableWidth)
            _hScrollOffset = cursorTextCol - availableWidth + 1;

        // Reduce offset when content (including room for the end-of-line cursor)
        // would otherwise leave blank space on the right.
        int const maxOffset = std::max(0, totalTextCols + 1 - availableWidth);
        _hScrollOffset = std::min(_hScrollOffset, maxOffset);
        _hScrollOffset = std::max(_hScrollOffset, 0);

        // Render pass with horizontal scroll applied.
        int textCol = 0;
        std::size_t graphemeIdx = 0;
        auto next = segmenter.begin();
        while (next != segmenter.end())
        {
            auto it = next;
            ++next;
            auto const clusterIndex = graphemeIdx;
            ++graphemeIdx;

            auto const& cluster = *it;
            char const* clusterStart = it._clusterStart;
            char const* clusterEnd =
                (next != segmenter.end()) ? next._clusterStart : (_buffer.data() + _buffer.size());
            auto const clusterByteStart = static_cast<std::size_t>(clusterStart - _buffer.data());
            int const clusterWidth = _masked ? 1 : graphemeClusterWidth(cluster);

            int const visibleStart = textCol - _hScrollOffset;
            int const canvasCol = textStartCol + visibleStart;

            // Fully clipped off the left.
            if (visibleStart + clusterWidth <= 0)
            {
                textCol += clusterWidth;
                continue;
            }
            // Fully clipped off the right.
            if (canvasCol >= width)
                break;

            // Build style: start with text style, apply decorator, then selection.
            Style style = textStyle;
            if (_textDecorator)
            {
                auto const pos =
                    TextPosition { .graphemeIndex = clusterIndex, .byteOffset = clusterByteStart };
                if (auto fg = _textDecorator->foreground(pos))
                    style.fg = *fg;
                if (auto ul = _textDecorator->underline(pos))
                {
                    style.underlineStyle = ul->style;
                    style.underlineColor = ul->color;
                }
            }
            if (hasSelection())
            {
                auto const selStart = selectionStart();
                auto const selEnd = selectionEnd();
                if (clusterByteStart >= selStart && clusterByteStart < selEnd)
                    style = selectionStyle;
            }

            // Apply background from decorator at the actual canvas column.
            if (_textDecorator)
            {
                if (auto bg = _textDecorator->background(std::max(textStartCol, canvasCol)))
                    style.bg = *bg;
            }

            if (visibleStart < 0)
            {
                // Wide cluster straddling the left edge: fill visible cells with spaces
                // so we don't overflow the prompt area.
                auto const fillEnd = std::min(canvasCol + clusterWidth, width);
                for (auto const c: std::views::iota(textStartCol, std::max(textStartCol, fillEnd)))
                    canvas.putString(0, c, " ", style);
            }
            else if (_masked)
            {
                canvas.putString(0, canvasCol, "*", style);
            }
            else
            {
                std::string_view const clusterView(clusterStart,
                                                   static_cast<std::size_t>(clusterEnd - clusterStart));
                canvas.putString(0, canvasCol, clusterView, style);
            }
            textCol += clusterWidth;
        }

        int const cursorDisplayCol = std::min(width - 1, textStartCol + (cursorTextCol - _hScrollOffset));

        int const textEndCol = textStartCol + (totalTextCols - _hScrollOffset);
        if (!_masked && !_ghostText.empty() && _cursor >= _buffer.size() && textEndCol < width
            && textEndCol >= textStartCol)
        {
            int ghostCol = textEndCol;
            renderGhostText(canvas, 0, ghostCol, ghostStyle);
        }

        // Scroll continuation indicators: render on top of the first/last text cell
        // when buffer content extends beyond the visible viewport. Style uses the
        // dim ghost style so the markers don't compete with the input itself.
        if (_hScrollOffset > 0)
            canvas.putString(0, textStartCol, "\xE2\x80\xB9", ghostStyle); // U+2039 ‹
        if (totalTextCols - _hScrollOffset > availableWidth)
        {
            int const indicatorCol = width - 1;
            if (indicatorCol >= textStartCol && indicatorCol != cursorDisplayCol)
                canvas.putString(0, indicatorCol, "\xE2\x80\xBA", ghostStyle); // U+203A ›
        }

        canvas.setCursor(0, cursorDisplayCol);
        return;
    }

    // ====================================================================
    // Multiline mode
    // ====================================================================
    auto const totalLines = lineCount();

    // Auto-scroll: ensure cursorLine is visible
    auto const curLine = cursorLine();
    if (curLine < _scrollOffset)
        _scrollOffset = curLine;
    else if (curLine >= _scrollOffset + height)
        _scrollOffset = curLine - height + 1;
    _scrollOffset = std::clamp(_scrollOffset, 0, std::max(0, totalLines - 1));

    // Get selection bounds once
    auto const hasSel = hasSelection();
    auto const selStart = selectionStart();
    auto const selEnd = selectionEnd();

    int cursorRow = 0;
    int cursorDisplayCol = 0;

    // Compute global grapheme index for the first visible line (skip graphemes before scrollOffset)
    std::size_t globalGraphemeBase = 0;
    if (_scrollOffset > 0)
    {
        auto const prefixLen = lineStartOffset(_scrollOffset);
        auto const prefix = std::string_view(_buffer.data(), prefixLen);
        auto seg = unicode::utf8_grapheme_segmenter(prefix);
        for ([[maybe_unused]] auto const& cluster: seg)
            ++globalGraphemeBase;
    }

    // Compute auto-continuation string when no explicit continuation prompt is set.
    // This pads continuation lines with spaces matching the prompt's display width,
    // so multiline text aligns with the first line's content.
    auto autoContinuation = std::string {};
    if (_continuationPrompt.empty() && !_prompt.empty())
    {
        auto seg = unicode::utf8_grapheme_segmenter(_prompt);
        int w = 0;
        for (auto const& cluster: seg)
            w += graphemeClusterWidth(cluster);
        autoContinuation.assign(static_cast<std::size_t>(w), ' ');
    }
    auto const& effectiveContinuation = _continuationPrompt.empty() ? autoContinuation : _continuationPrompt;

    auto const lastVisibleLine = std::min(totalLines, _scrollOffset + height);
    for (auto const lineIndex: std::views::iota(_scrollOffset, std::max(_scrollOffset, lastVisibleLine)))
    {
        auto const row = lineIndex - _scrollOffset;
        auto const lineContent = lineAt(lineIndex);
        auto const lineStartByte = lineStartOffset(lineIndex);
        auto const lineEndByte = lineStartByte + lineContent.size();

        int col = 0;

        // Fill background per cell (decorator or default)
        for (auto const c: std::views::iota(0, std::max(0, width)))
        {
            Style cellStyle = _styles.background.value_or(Style {});
            if (_textDecorator)
            {
                if (auto bg = _textDecorator->background(c))
                    cellStyle.bg = *bg;
            }
            canvas.put(row, c, " ", cellStyle);
        }

        // Render prompt (first visible line) or continuation prompt with per-column decorator bg
        {
            auto const& promptStr = (lineIndex == 0) ? _prompt : effectiveContinuation;
            if (!promptStr.empty())
            {
                auto promptSeg = unicode::utf8_grapheme_segmenter(promptStr);
                auto pNext = promptSeg.begin();
                while (pNext != promptSeg.end() && col < width)
                {
                    auto const pIt = pNext;
                    ++pNext;
                    char const* pStart = pIt._clusterStart;
                    char const* pEnd = (pNext != promptSeg.end()) ? pNext._clusterStart
                                                                  : (promptStr.data() + promptStr.size());
                    Style pStyle = textStyle;
                    if (_textDecorator)
                    {
                        if (auto bg = _textDecorator->background(col))
                            pStyle.bg = *bg;
                    }
                    auto const pView = std::string_view(pStart, static_cast<std::size_t>(pEnd - pStart));
                    col += canvas.putString(row, col, pView, pStyle);
                }
            }
        }
        auto const promptDisplayWidth = col; // Display columns consumed by prompt/continuation

        // Determine selection range local to this line
        auto const lineSelStart = (hasSel && selStart < lineEndByte && selEnd > lineStartByte)
                                      ? std::max(selStart, lineStartByte) - lineStartByte
                                      : lineContent.size();
        auto const lineSelEnd = (hasSel && selStart < lineEndByte && selEnd > lineStartByte)
                                    ? std::min(selEnd, lineEndByte) - lineStartByte
                                    : lineContent.size();

        // Render line content grapheme by grapheme
        auto segmenter = unicode::utf8_grapheme_segmenter(lineContent);
        std::size_t localByteOffset = 0;
        std::size_t lineGraphemeCount = 0;

        auto next = segmenter.begin();
        while (next != segmenter.end() && col < width)
        {
            auto it = next;
            ++next;
            auto const clusterIndex = lineGraphemeCount;
            ++lineGraphemeCount;

            auto const& cluster = *it;
            char const* clusterStart = it._clusterStart;
            char const* clusterEnd =
                (next != segmenter.end()) ? next._clusterStart : (lineContent.data() + lineContent.size());
            localByteOffset = static_cast<std::size_t>(clusterStart - lineContent.data());
            auto const globalByteOffset = lineStartByte + localByteOffset;

            int const clusterWidth = graphemeClusterWidth(cluster);

            Style style = textStyle;
            if (_textDecorator)
            {
                // Both graphemeIndex and byteOffset are buffer-global.
                auto const pos = TextPosition { .graphemeIndex = globalGraphemeBase + clusterIndex,
                                                .byteOffset = globalByteOffset };
                if (auto fg = _textDecorator->foreground(pos))
                    style.fg = *fg;
                if (auto ul = _textDecorator->underline(pos))
                {
                    style.underlineStyle = ul->style;
                    style.underlineColor = ul->color;
                }
            }

            // Selection highlighting
            if (localByteOffset >= lineSelStart && localByteOffset < lineSelEnd)
                style.inverse = true;

            // Background from decorator
            if (_textDecorator)
            {
                if (auto bg = _textDecorator->background(col))
                    style.bg = *bg;
            }

            std::string_view const clusterView(clusterStart,
                                               static_cast<std::size_t>(clusterEnd - clusterStart));
            canvas.putString(row, col, clusterView, style);
            col += clusterWidth;
        }

        // Advance global grapheme base: line graphemes + 1 for newline (except last line)
        globalGraphemeBase += lineGraphemeCount;
        if (lineIndex < totalLines - 1)
            ++globalGraphemeBase; // Account for newline grapheme cluster

        // Ghost text on last line only, when cursor is at end of buffer
        if (lineIndex == totalLines - 1 && !_ghostText.empty() && _cursor >= _buffer.size())
            renderGhostText(canvas, row, col, ghostStyle);

        // Track cursor position for this line
        if (lineIndex == curLine)
        {
            cursorRow = row;
            auto const cursorInLine = _cursor - lineStartByte;
            auto curCol = promptDisplayWidth;
            auto seg = unicode::utf8_grapheme_segmenter(lineContent);
            auto curIt = seg.begin();
            while (curIt != seg.end())
            {
                auto const localOffset = static_cast<std::size_t>(curIt._clusterStart - lineContent.data());
                if (localOffset >= cursorInLine)
                    break;
                curCol += graphemeClusterWidth(*curIt);
                ++curIt;
            }
            cursorDisplayCol = curCol;
        }
    }

    canvas.setCursor(cursorRow, cursorDisplayCol);
}

EventResult InputField::onEvent(InputEvent const& event)
{
    auto action = processEvent(event);

    switch (action)
    {
        case InputFieldAction::Changed: invalidate(); return EventResult::Handled;
        case InputFieldAction::Submit:
        case InputFieldAction::Abort:
        case InputFieldAction::Eof:
        case InputFieldAction::AgentMode:
        case InputFieldAction::CycleAgentMode:
        case InputFieldAction::CycleThinkingMode:
        case InputFieldAction::CycleModel:
        case InputFieldAction::CommandPalette:
        case InputFieldAction::FuzzyFileFinder:
        case InputFieldAction::NewPrompt:
            // These need special handling by the parent
            return EventResult::Handled;
        case InputFieldAction::None: return EventResult::Ignored;
    }

    return EventResult::Ignored;
}

Size InputField::preferredSize() const
{
    // Calculate height based on line count
    int lines = lineCount();
    if (_maxLines > 0)
        lines = std::min(lines, _maxLines);

    // Width: prompt + some reasonable text width
    int const promptWidth = static_cast<int>(_prompt.size());
    return { .width = promptWidth + 40, .height = std::max(1, lines) };
}

auto InputField::text() const noexcept -> std::string_view
{
    return _buffer;
}

auto InputField::cursor() const noexcept -> std::size_t
{
    return _cursor;
}

void InputField::setPrompt(std::string_view prompt)
{
    _prompt = std::string(prompt);
}

auto InputField::prompt() const noexcept -> std::string_view
{
    return _prompt;
}

void InputField::setStyles(InputFieldStyles styles)
{
    _styles = styles;
}

void InputField::clear()
{
    _buffer.clear();
    _cursor = 0;
    _hScrollOffset = 0;
    clearSelection();
    // A wholesale buffer replacement invalidates the ghost state; drop BOTH the displayed
    // suggestion and the consumed-prefix memory. Clearing only _ghostConsumed would leave a
    // stale _ghostText rendering over (or being re-prepended onto) the new buffer.
    clearGhostText();
}

void InputField::setText(std::string_view text)
{
    _buffer = _multiline ? std::string(text) : sanitizeForSingleLine(text);
    _cursor = _buffer.size();
    _hScrollOffset = 0;
    clearSelection();
    // Wholesale buffer replacement: drop both the displayed suggestion and the consumed memory
    // (see clear()), otherwise a previous buffer's ghost renders/restores on the new text.
    clearGhostText();
}

void InputField::setCursor(size_t pos)
{
    _cursor = std::min(pos, _buffer.size());
    clearSelection();
}

void InputField::addHistory(std::string entry)
{
    if (entry.empty())
        return;
    // Avoid consecutive duplicates
    if (!_history.empty() && _history.back() == entry)
        return;
    _history.push_back(std::move(entry));
    if (_history.size() > _maxHistory)
        _history.erase(_history.begin());
    _historyIndex = _history.size();
}

void InputField::setMaxHistory(std::size_t n)
{
    _maxHistory = n;
    while (_history.size() > _maxHistory)
        _history.erase(_history.begin());
}

void InputField::setKeyBindings(KeyBindings bindings)
{
    _keyBindings = std::move(bindings);
}

auto InputField::keyBindings() const noexcept -> KeyBindings const&
{
    return _keyBindings;
}

auto InputField::keyBindings() noexcept -> KeyBindings&
{
    return _keyBindings;
}

auto InputField::executeAction(EditAction action) -> InputFieldAction
{
    switch (action)
    {
        case EditAction::None: return InputFieldAction::None;

        // Movement
        case EditAction::MoveForwardChar:
            clearSelection();
            moveForwardChar();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::MoveBackwardChar:
            clearSelection();
            moveBackwardChar();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::MoveForwardWord:
            clearSelection();
            moveForwardWord();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::MoveBackwardWord:
            clearSelection();
            moveBackwardWord();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::MoveToLineStart:
            clearSelection();
            if (_multiline)
                moveToLineStart();
            else
                moveToStart();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::MoveToLineEnd:
            clearSelection();
            if (_multiline)
                moveToLineEnd();
            else
                moveToEnd();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::MoveToBufferStart:
            clearSelection();
            moveToBufferStart();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::MoveToBufferEnd:
            clearSelection();
            moveToBufferEnd();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::MoveUp:
            clearSelection();
            if (_multiline)
                moveUp();
            else
                historyPrev();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::MoveDown:
            clearSelection();
            if (_multiline)
                moveDown();
            else
                historyNext();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::SmartMoveToLineStart:
            clearSelection();
            smartMoveToLineStart();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::SmartMoveToLineEnd:
            clearSelection();
            smartMoveToLineEnd();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        // Editing
        case EditAction::DeleteCharBackward:
            if (hasSelection())
            {
                clearGhostText(); // Selection delete is not a prefix-shortening edit.
                saveUndoState();
                deleteSelection();
            }
            else
            {
                deleteCharBackward(); // Adjusts ghost text internally (re-prepend or clear).
            }
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::DeleteCharForward:
            if (hasSelection())
            {
                clearGhostText(); // Selection delete is not a prefix-shortening edit.
                saveUndoState();
                deleteSelection();
            }
            else
            {
                deleteChar(); // Forward delete: clears ghost only if it actually deletes.
            }
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::DeleteWord:
            killWord(); // Forward word kill: clears ghost only if it actually deletes.
            return InputFieldAction::Changed;

        case EditAction::DeleteWordBackward:
            killWordBackward(); // Adjusts ghost text internally (re-prepend or clear).
            return InputFieldAction::Changed;

        case EditAction::DeleteBigWordBackward:
            killBigWordBackward(); // Adjusts ghost text internally (re-prepend or clear).
            return InputFieldAction::Changed;

        case EditAction::KillToEnd:
            killToEnd(); // Forward kill: clears ghost only if it actually deletes.
            return InputFieldAction::Changed;

        case EditAction::KillToStart:
            killToStart(); // Adjusts ghost text internally (re-prepend or clear).
            return InputFieldAction::Changed;

        case EditAction::Transpose:
            transpose();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::ClearBuffer:
            clearGhostText();
            _lastWasKill = false;
            clear();
            return InputFieldAction::Changed;

        // Undo/Redo
        case EditAction::Undo:
            _lastWasKill = false;
            if (undo())
            {
                // The snapshot restores buffer+cursor only; a leftover ghost would duplicate
                // restored text and could be committed on accept. Drop it; the consumer recomputes.
                clearGhostText();
                return InputFieldAction::Changed;
            }
            return InputFieldAction::None;

        case EditAction::Redo:
            _lastWasKill = false;
            if (redo())
            {
                clearGhostText();
                return InputFieldAction::Changed;
            }
            return InputFieldAction::None;

        // Kill Ring
        case EditAction::Yank:
            yank();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::YankPop:
            yankPop();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        // Selection
        case EditAction::SelectAll:
            selectAll();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        // Clipboard
        case EditAction::Copy:
            if (hasSelection())
            {
                copySelection();
                _lastWasKill = false;
                return InputFieldAction::Changed;
            }
            return InputFieldAction::None;

        case EditAction::Cut:
            if (hasSelection())
            {
                saveUndoState();
                cutSelection();
                _lastWasKill = false;
                return InputFieldAction::Changed;
            }
            return InputFieldAction::None;

        case EditAction::Paste:
            // Paste from kill ring (system clipboard handled via PasteEvent)
            yank();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        // Control
        case EditAction::Submit:
            clearSelection();
            _lastWasKill = false;
            return InputFieldAction::Submit;

        case EditAction::Abort: _lastWasKill = false; return InputFieldAction::Abort;

        case EditAction::NewPrompt:
            clearSelection();
            _lastWasKill = false;
            return InputFieldAction::NewPrompt;

        case EditAction::InsertNewline:
            if (_multiline)
            {
                saveUndoState();
                clearGhostText();
                if (hasSelection())
                    deleteSelection();
                insertNewline();
                _lastWasKill = false;
                return InputFieldAction::Changed;
            }
            return InputFieldAction::None;

        case EditAction::AgentMode: _lastWasKill = false; return InputFieldAction::AgentMode;

        case EditAction::CycleAgentMode: _lastWasKill = false; return InputFieldAction::CycleAgentMode;

        case EditAction::CycleThinkingMode: _lastWasKill = false; return InputFieldAction::CycleThinkingMode;

        case EditAction::CycleModel: _lastWasKill = false; return InputFieldAction::CycleModel;

        case EditAction::CommandPalette: _lastWasKill = false; return InputFieldAction::CommandPalette;

        case EditAction::FuzzyFileFinder: _lastWasKill = false; return InputFieldAction::FuzzyFileFinder;

        // History
        case EditAction::HistoryPrev:
            clearSelection();
            historyPrev();
            _lastWasKill = false;
            return InputFieldAction::Changed;

        case EditAction::HistoryNext:
            clearSelection();
            historyNext();
            _lastWasKill = false;
            return InputFieldAction::Changed;
    }

    return InputFieldAction::None;
}

auto InputField::handleKey(KeyEvent const& key) -> InputFieldAction
{
    auto const ctrl = hasModifier(key.modifiers, Modifier::Ctrl);
    auto const alt = hasModifier(key.modifiers, Modifier::Alt);
    auto const shift = hasModifier(key.modifiers, Modifier::Shift);

    // ========================================================================
    // Special case: Shift+movement keys for selection extension
    // These cannot be handled by the keybinding system because they modify
    // the behavior of movement keys rather than being separate actions.
    // ========================================================================
    if (shift && !ctrl && !alt)
    {
        switch (key.key)
        {
            case KeyCode::Up:
                _lastWasKill = false;
                if (_multiline)
                    moveWithSelection(&InputField::moveUp);
                else
                    moveWithSelection(&InputField::historyPrev);
                return InputFieldAction::Changed;

            case KeyCode::Down:
                _lastWasKill = false;
                if (_multiline)
                    moveWithSelection(&InputField::moveDown);
                else
                    moveWithSelection(&InputField::historyNext);
                return InputFieldAction::Changed;

            case KeyCode::Left:
                _lastWasKill = false;
                moveWithSelection(&InputField::moveBackwardChar);
                return InputFieldAction::Changed;

            case KeyCode::Right:
                _lastWasKill = false;
                moveWithSelection(&InputField::moveForwardChar);
                return InputFieldAction::Changed;

            case KeyCode::Home:
                _lastWasKill = false;
                if (_multiline)
                    moveWithSelection(&InputField::moveToLineStart);
                else
                    moveWithSelection(&InputField::moveToStart);
                return InputFieldAction::Changed;

            case KeyCode::End:
                _lastWasKill = false;
                if (_multiline)
                    moveWithSelection(&InputField::moveToLineEnd);
                else
                    moveWithSelection(&InputField::moveToEnd);
                return InputFieldAction::Changed;

            default: break;
        }
    }

    // Shift+Ctrl movement (select word)
    if (shift && ctrl && !alt)
    {
        switch (key.key)
        {
            case KeyCode::Left:
                _lastWasKill = false;
                moveWithSelection(&InputField::moveBackwardWord);
                return InputFieldAction::Changed;

            case KeyCode::Right:
                _lastWasKill = false;
                moveWithSelection(&InputField::moveForwardWord);
                return InputFieldAction::Changed;

            case KeyCode::Home:
                _lastWasKill = false;
                moveWithSelection(&InputField::moveToBufferStart);
                return InputFieldAction::Changed;

            case KeyCode::End:
                _lastWasKill = false;
                moveWithSelection(&InputField::moveToBufferEnd);
                return InputFieldAction::Changed;

            default: break;
        }
    }

    // ========================================================================
    // Special case: Context-dependent keys
    // ========================================================================

    // Ctrl+D: EOF if buffer empty, delete char forward otherwise
    if (ctrl && !alt && !shift && key.codepoint == 'd')
    {
        if (_buffer.empty())
            return InputFieldAction::Eof;
        // Otherwise fall through to keybinding lookup (DeleteCharForward)
    }

    // Plain Tab: Not handled yet, ignore (modified Tab like Shift+Tab goes to keybinding lookup)
    if (key.key == KeyCode::Tab && !shift && !ctrl && !alt)
    {
        _lastWasKill = false;
        return InputFieldAction::None;
    }

    // ========================================================================
    // Keybinding lookup
    // ========================================================================
    if (auto action = _keyBindings.lookup(key); action && *action != EditAction::None)
    {
        return executeAction(*action);
    }

    // ========================================================================
    // Printable characters
    // ========================================================================
    if (key.codepoint != 0 && !ctrl && !alt && isTextProducingKey(key.key))
    {
        // Save undo state before any changes
        saveUndoState();

        // Delete selection first if any (typing replaces selection)
        if (hasSelection())
        {
            clearGhostText();
            deleteSelection();
        }

        auto cp = key.codepoint;
        // Handle Shift and CapsLock modifiers for letter capitalization
        // Kitty keyboard protocol sends lowercase codepoint + modifier flags
        // CapsLock XOR Shift: either one (but not both) should uppercase letters
        bool const capsActive = hasModifier(key.modifiers, Modifier::CapsLock);
        bool const shouldCapitalize = (shift != capsActive); // XOR logic
        if (shouldCapitalize && cp >= 'a' && cp <= 'z')
            cp = cp - 'a' + 'A';

        // Smart ghost text trimming: if typing at end and character matches ghost prefix, trim it;
        // otherwise clear the now-stale ghost text. This avoids the visual flicker caused by
        // clearing ghost text immediately and only recomputing it after the debounce delay.
        if (!_ghostText.empty() && _cursor == _buffer.size())
        {
            auto const typed = encodeUtf8(cp);
            if (_ghostText.starts_with(typed))
            {
                // Matching keystroke: eat the char off the ghost head and remember it. Should the
                // very next edit be a backspace of this char, adjustGhostAfterBackwardDelete()
                // restores the suggestion from _ghostConsumed rather than waiting for the debounced
                // recompute. This is what kills the first-deletion flicker when the typed char
                // trims the ghost all the way to empty.
                _ghostConsumed += typed;
                _ghostText.erase(0, typed.size());
            }
            else
            {
                clearGhostText();
            }
        }
        else
        {
            clearGhostText();
        }

        insertCodepoint(cp);
        _lastWasKill = false;
        return InputFieldAction::Changed;
    }

    return InputFieldAction::None;
}

void InputField::killToEnd()
{
    // Forward kill: clear unconditionally. When the cursor is at end (nothing to kill) the ghost
    // is showing but no longer valid; mid-buffer it was never rendered. Either way, drop it.
    clearGhostText();
    if (_cursor < _buffer.size())
    {
        saveUndoState();
        auto killed = _buffer.substr(_cursor);
        _buffer.erase(_cursor);
        pushKillRing(std::move(killed));
    }
    _lastWasKill = true;
}

void InputField::killToStart()
{
    if (_cursor > 0)
    {
        bool const wasAtEnd = _cursor == _buffer.size();
        saveUndoState();
        auto killed = _buffer.substr(0, _cursor);
        _buffer.erase(0, _cursor);
        _cursor = 0;
        adjustGhostAfterBackwardDelete(killed, wasAtEnd);
        pushKillRing(std::move(killed));
    }
    else
    {
        // Cursor-at-start no-op: drop any stale ghost (cleared unconditionally pre-commit).
        clearGhostText();
    }
    _lastWasKill = true;
}

void InputField::killWord()
{
    // Forward kill: clear unconditionally. At end-of-buffer (no word ahead) the kill is a no-op
    // but the ghost is showing and stale; mid-buffer it was never rendered.
    clearGhostText();
    auto const start = _cursor;
    // Find end position without modifying cursor
    auto endPos = _cursor;
    auto const size = _buffer.size();
    while (endPos < size && !isWordChar(static_cast<unsigned char>(_buffer[endPos])))
        endPos = nextGraphemeCluster(endPos);
    while (endPos < size && isWordChar(static_cast<unsigned char>(_buffer[endPos])))
        endPos = nextGraphemeCluster(endPos);

    if (endPos > start)
    {
        saveUndoState();
        auto killed = _buffer.substr(start, endPos - start);
        _buffer.erase(start, endPos - start);
        pushKillRing(std::move(killed));
    }
    _lastWasKill = true;
}

void InputField::killWordBackward()
{
    killWordBackwardWith(&InputField::isWordChar);
}

void InputField::killBigWordBackward()
{
    killWordBackwardWith(&InputField::isBigWordChar);
}

void InputField::killWordBackwardWith(bool (*isWordCharFn)(char32_t))
{
    auto const end = _cursor;
    bool const wasAtEnd = _cursor == _buffer.size();
    // Find start position without modifying cursor. Walk by codepoint (prevUtf8 is O(1), unlike
    // prevGraphemeCluster which re-segments the whole prefix on every call) and classify the whole
    // decoded codepoint so multibyte separators such as U+3000 are recognized as boundaries.
    auto startPos = _cursor;
    while (startPos > 0 && !isWordCharFn(decodeUtf8At(_buffer, prevUtf8(_buffer, startPos))))
        startPos = prevUtf8(_buffer, startPos);
    while (startPos > 0 && isWordCharFn(decodeUtf8At(_buffer, prevUtf8(_buffer, startPos))))
        startPos = prevUtf8(_buffer, startPos);

    if (startPos < end)
    {
        saveUndoState();
        auto killed = _buffer.substr(startPos, end - startPos);
        _buffer.erase(startPos, end - startPos);
        _cursor = startPos;
        adjustGhostAfterBackwardDelete(killed, wasAtEnd);
        pushKillRing(std::move(killed));
    }
    else
    {
        // Cursor-at-start no-op: drop any stale ghost (cleared unconditionally pre-commit).
        clearGhostText();
    }
    _lastWasKill = true;
}

void InputField::yank()
{
    if (_killRing.empty())
        return;
    saveUndoState();
    // Yank rewrites the buffer tail; any displayed suggestion and consumed-prefix memory now
    // belong to the pre-yank text. Drop both so a later backspace can't restore a stale ghost.
    clearGhostText();
    _killRingIndex = _killRing.size() - 1;
    insertText(_killRing[_killRingIndex]);
}

void InputField::yankPop()
{
    if (_killRing.empty())
        return;
    saveUndoState();
    // Yank-pop rewrites the buffer tail (see yank()): invalidate the stale ghost state.
    clearGhostText();
    // Remove the previously yanked text and insert the next one in the ring
    if (_killRingIndex < _killRing.size())
    {
        auto const& prev = _killRing[_killRingIndex];
        if (_cursor >= prev.size())
        {
            _buffer.erase(_cursor - prev.size(), prev.size());
            _cursor -= prev.size();
        }
    }
    _killRingIndex = (_killRingIndex == 0) ? _killRing.size() - 1 : _killRingIndex - 1;
    insertText(_killRing[_killRingIndex]);
}

void InputField::deleteChar()
{
    if (_cursor >= _buffer.size())
        return; // Nothing deleted at end-of-buffer: a visible ghost (if any) stays valid.
    saveUndoState();
    auto const next = nextGraphemeCluster(_cursor);
    _buffer.erase(_cursor, next - _cursor);
    clearGhostText(); // Mid-buffer delete: cursor not at end, ghost was not rendered.
}

void InputField::deleteCharBackward()
{
    if (_cursor == 0)
    {
        clearGhostText(); // Cursor-at-start no-op: drop any stale ghost (cleared unconditionally pre-commit).
        return;
    }
    bool const wasAtEnd = _cursor == _buffer.size();
    saveUndoState();
    auto const prev = prevGraphemeCluster(_cursor);
    // Only materialize the deleted run when it can feed the ghost: either a live ghost to
    // re-prepend onto, or a consumed-prefix memory to restore from (the trimmed-to-empty case).
    // The common no-suggestion backspace then allocates nothing.
    auto const removed = (wasAtEnd && (!_ghostText.empty() || !_ghostConsumed.empty()))
                             ? _buffer.substr(prev, _cursor - prev)
                             : std::string {};
    _buffer.erase(prev, _cursor - prev);
    _cursor = prev;
    adjustGhostAfterBackwardDelete(removed, wasAtEnd);
}

void InputField::moveToStart()
{
    _cursor = 0;
}

void InputField::moveToEnd()
{
    _cursor = _buffer.size();
}

void InputField::moveForwardChar()
{
    _cursor = nextGraphemeCluster(_cursor);
}

void InputField::moveBackwardChar()
{
    _cursor = prevGraphemeCluster(_cursor);
}

void InputField::moveForwardWord()
{
    auto const size = _buffer.size();
    // Emacs forward-word: skip non-word chars, then skip word chars
    while (_cursor < size && !isWordChar(decodeUtf8At(_buffer, _cursor)))
        _cursor = nextUtf8(_buffer, _cursor);
    while (_cursor < size && isWordChar(decodeUtf8At(_buffer, _cursor)))
        _cursor = nextUtf8(_buffer, _cursor);
}

void InputField::moveBackwardWord()
{
    // Skip whitespace/non-word characters
    while (_cursor > 0 && !isWordChar(decodeUtf8At(_buffer, prevUtf8(_buffer, _cursor))))
        _cursor = prevUtf8(_buffer, _cursor);
    // Skip word characters
    while (_cursor > 0 && isWordChar(decodeUtf8At(_buffer, prevUtf8(_buffer, _cursor))))
        _cursor = prevUtf8(_buffer, _cursor);
}

void InputField::historyPrev()
{
    if (_history.empty())
        return;
    if (_historyIndex == _history.size())
        _savedLine = _buffer;
    if (_historyIndex > 0)
    {
        --_historyIndex;
        _buffer = _multiline ? _history[_historyIndex] : sanitizeForSingleLine(_history[_historyIndex]);
        _cursor = _buffer.size();
        clearSelection();
        // Recalled entry replaces the buffer wholesale (bypassing setText); drop the previous
        // buffer's ghost state so a later backspace can't restore a suggestion from it.
        clearGhostText();
    }
}

void InputField::historyNext()
{
    if (_historyIndex >= _history.size())
        return;
    ++_historyIndex;
    if (_historyIndex == _history.size())
    {
        _buffer = _savedLine;
        _savedLine.clear();
    }
    else
    {
        _buffer = _multiline ? _history[_historyIndex] : sanitizeForSingleLine(_history[_historyIndex]);
    }
    _cursor = _buffer.size();
    clearSelection();
    // Recalled entry replaces the buffer wholesale (see historyPrev): invalidate stale ghost state.
    clearGhostText();
}

void InputField::transpose()
{
    if (_cursor == 0 || _buffer.size() < 2)
        return;
    saveUndoState();
    // Transpose rewrites characters at the buffer tail; any displayed suggestion / consumed-prefix
    // memory no longer matches. Drop them so a later backspace can't restore a stale ghost.
    clearGhostText();
    // If at end, transpose the two characters before cursor
    auto pos = _cursor;
    if (pos == _buffer.size())
        pos = prevGraphemeCluster(pos);
    auto const prevPos = prevGraphemeCluster(pos);
    auto const nextPos = nextGraphemeCluster(pos);

    auto first = _buffer.substr(prevPos, pos - prevPos);
    auto second = _buffer.substr(pos, nextPos - pos);

    _buffer.replace(prevPos, nextPos - prevPos, second + first);
    _cursor = nextPos;
}

void InputField::pushKillRing(std::string text)
{
    if (text.empty())
        return;
    if (_lastWasKill && !_killRing.empty())
    {
        // Append to the last kill ring entry
        _killRing.back() += text;
    }
    else
    {
        _killRing.push_back(std::move(text));
        if (_killRing.size() > MaxKillRing)
            _killRing.erase(_killRing.begin());
    }
}

void InputField::insertCodepoint(char32_t cp)
{
    auto const utf8 = encodeUtf8(cp);
    _buffer.insert(_cursor, utf8);
    _cursor += utf8.size();
}

void InputField::insertText(std::string_view text)
{
    if (!_multiline)
    {
        auto const sanitized = sanitizeForSingleLine(text);
        _buffer.insert(_cursor, sanitized);
        _cursor += sanitized.size();
    }
    else
    {
        _buffer.insert(_cursor, text);
        _cursor += text.size();
    }
}

auto InputField::nextGraphemeCluster(std::size_t pos) const -> std::size_t
{
    if (pos >= _buffer.size())
        return pos;

    // Use libunicode utf8_grapheme_segmenter for proper grapheme cluster boundaries.
    // The iterator's _clusterStart pointer tracks byte positions in the original string_view.
    auto const sv = std::string_view(_buffer).substr(pos);
    auto segmenter = unicode::utf8_grapheme_segmenter(sv);
    auto it = segmenter.begin();
    if (it == segmenter.end())
        return nextUtf8(_buffer, pos);

    ++it; // Advance past the first grapheme cluster
    if (it != segmenter.end())
    {
        // The iterator's _clusterStart points into the original sv data
        auto const byteOffset = static_cast<std::size_t>(it._clusterStart - sv.data());
        return pos + byteOffset;
    }

    // The first cluster spans to the end
    return _buffer.size();
}

auto InputField::prevGraphemeCluster(std::size_t pos) const -> std::size_t
{
    if (pos == 0)
        return 0;

    // Segment the text up to pos and find the last cluster boundary.
    auto const sv = std::string_view(_buffer).substr(0, pos);
    auto segmenter = unicode::utf8_grapheme_segmenter(sv);

    auto lastBoundaryOffset = std::size_t { 0 };
    auto it = segmenter.begin();
    while (it != segmenter.end())
    {
        lastBoundaryOffset = static_cast<std::size_t>(it._clusterStart - sv.data());
        ++it;
    }

    return lastBoundaryOffset;
}

auto InputField::isWordChar(char32_t c) -> bool
{
    // Fish-style word boundaries: alphanumeric and underscore are word characters.
    // Path separators, punctuation, and whitespace are boundaries.
    if (c < 0x80 && std::isalnum(static_cast<unsigned char>(c)))
        return true;
    if (c == U'_')
        return true;
    // Everything else (including '/', '.', '-', whitespace, etc.) is a boundary
    return false;
}

auto InputField::isBigWordChar(char32_t c) -> bool
{
    // Fish-style "bigword" boundaries: only whitespace separates words. Every non-whitespace
    // character (including '/', '.', '-', and punctuation) is part of the word, so a backward
    // kill removes a whole space-delimited token (e.g. all of "git checkout -b" at once).
    return !isUnicodeWhitespace(c);
}

// ============================================================================
// Multiline support
// ============================================================================

void InputField::setTextDecorator(TextDecorator const* decorator)
{
    _textDecorator = decorator;
}

void InputField::setContinuationPrompt(std::string_view prompt)
{
    _continuationPrompt = std::string(prompt);
}

auto InputField::continuationPrompt() const noexcept -> std::string_view
{
    return _continuationPrompt;
}

auto InputField::lineStartOffset(int lineIndex) const -> std::size_t
{
    if (lineIndex <= 0)
        return 0;

    std::size_t pos = 0;
    int currentLine = 0;
    while (pos < _buffer.size() && currentLine < lineIndex)
    {
        if (_buffer[pos] == '\n')
            ++currentLine;
        ++pos;
    }
    return pos;
}

void InputField::setMasked(bool masked)
{
    _masked = masked;
}

auto InputField::isMasked() const noexcept -> bool
{
    return _masked;
}

void InputField::setMultiline(bool enable)
{
    _multiline = enable;
}

auto InputField::isMultiline() const noexcept -> bool
{
    return _multiline;
}

auto InputField::lineCount() const noexcept -> int
{
    if (_buffer.empty())
        return 1;

    int count = 1;
    for (char const c: _buffer)
    {
        if (c == '\n')
            ++count;
    }
    return count;
}

auto InputField::cursorLine() const noexcept -> int
{
    int line = 0;
    for (auto const c: std::string_view(_buffer).substr(0, std::min(_cursor, _buffer.size())))
    {
        if (c == '\n')
            ++line;
    }
    return line;
}

auto InputField::cursorColumn() const noexcept -> int
{
    auto const lineStart = findLineStart(_cursor);
    return countGraphemesInRange(lineStart, _cursor);
}

auto InputField::lineAt(int lineIndex) const -> std::string_view
{
    if (lineIndex < 0)
        return {};

    std::size_t start = 0;
    int currentLine = 0;

    // Find start of requested line
    while (currentLine < lineIndex && start < _buffer.size())
    {
        if (_buffer[start] == '\n')
            ++currentLine;
        ++start;
    }

    if (currentLine < lineIndex)
        return {}; // Line index out of range

    // Find end of line
    std::size_t end = start;
    while (end < _buffer.size() && _buffer[end] != '\n')
        ++end;

    return std::string_view(_buffer).substr(start, end - start);
}

void InputField::setMaxLines(int maxLines)
{
    _maxLines = maxLines;
}

auto InputField::maxLines() const noexcept -> int
{
    return _maxLines;
}

// ============================================================================
// Ghost text
// ============================================================================

void InputField::setGhostText(std::string_view ghost)
{
    _ghostText = std::string(firstGhostLine(ghost));
    // A freshly supplied suggestion supersedes any consumed-prefix memory: the restore-on-backspace
    // shortcut only makes sense relative to the ghost that was actually being shown.
    _ghostConsumed.clear();
}

void InputField::clearGhostText()
{
    _ghostText.clear();
    _ghostConsumed.clear();
}

void InputField::prependGhostText(std::string_view deleted)
{
    // Guard on a non-empty ghost so we never invent a suggestion that wasn't showing.
    if (_ghostText.empty() || deleted.empty())
        return;
    // The ghost is single-line (see firstGhostLine): a deleted line break (multiline buffer) must
    // not enter the suggestion, where it would render as a control glyph yet be committed verbatim.
    deleted = firstGhostLine(deleted);
    if (deleted.empty())
        return;
    _ghostText.insert(0, deleted);
}

void InputField::adjustGhostAfterBackwardDelete(std::string_view deletedRun, bool wasAtEnd)
{
    // Re-prepend only when the deletion was at the buffer end (where the ghost renders) and
    // text still remains. A mid-buffer delete isn't rendered, and a delete that empties the
    // buffer must not resurrect the just-killed line as a suggestion.
    if (wasAtEnd && !_buffer.empty())
    {
        // First-deletion case: typing the tail of a suggestion trims the ghost to empty (e.g. the
        // last 't' of "git checkout"), so there is no live ghost to re-prepend onto. If the just
        // deleted run is exactly the run we consumed off the ghost head while typing, undo that
        // consumption: restore the run onto the ghost and pop it from the memory. This reconstructs
        // the suggestion the debounce would recompute, with zero flicker.
        if (!_ghostConsumed.empty() && _ghostConsumed.ends_with(deletedRun))
        {
            _ghostText.insert(0, deletedRun);
            _ghostConsumed.erase(_ghostConsumed.size() - deletedRun.size());
        }
        else if (!_ghostConsumed.empty())
        {
            // We are in the consumed-memory regime (chars were eaten off the ghost head or a
            // suffix was accepted), but the deleted run is NOT a tail of that memory. Two ways to
            // get here, both un-restorable:
            //   1. A single deleted grapheme cluster blends an un-consumed base character with a
            //      consumed combining mark, so the cluster is longer than _ghostConsumed.
            //   2. A backward kill (notably the whitespace-delimited "bigword" kill bound to
            //      Alt+Backspace) removes more than the accepted/consumed suggestion tail — i.e.
            //      user-typed buffer text that preceded the suggestion is gone too.
            // In both cases the suggestion is no longer valid for the new buffer prefix: prepending
            // the deleted run verbatim would splice text that was never part of the suggestion into
            // the ghost (and commit it on accept), and the typed context the suggestion depended on
            // has been deleted. Don't guess; clear and let the debounce recompute.
            clearGhostText();
        }
        else
        {
            prependGhostText(deletedRun);
        }
    }
    else
    {
        clearGhostText();
    }
}

void InputField::acceptGhostText()
{
    if (_ghostText.empty())
        return;

    saveUndoState();
    // Remember the accepted suffix as the consumed-prefix memory (same role as the run eaten by
    // matching keystrokes): the whole suggestion is now in the buffer, so a backward delete of a
    // tail of it can restore the ghost synchronously via adjustGhostAfterBackwardDelete, with no
    // debounce gap. This is what kills the flicker when the user accepts (e.g. Ctrl+E) and then
    // word-deletes. Setting the members directly — NOT clearGhostText() — preserves the seed.
    //
    // The seed MUST be the bytes that actually landed in the buffer, not the raw ghost: in
    // single-line mode insertText runs sanitizeForSingleLine (strips '\r', maps '\n'→space). If the
    // seed kept the raw bytes, a later backward delete materializes deletedRun from the sanitized
    // buffer and _ghostConsumed.ends_with(deletedRun) would mismatch, skipping the synchronous
    // restore (a flicker). Capture the buffer tail post-insert so seed and buffer agree byte-for-byte.
    auto const before = _cursor;
    insertText(_ghostText);
    _ghostText.clear();
    _ghostConsumed = _buffer.substr(before, _cursor - before);
}

auto InputField::ghostText() const noexcept -> std::string_view
{
    return _ghostText;
}

auto InputField::hasGhostText() const noexcept -> bool
{
    return !_ghostText.empty();
}

void InputField::renderGhostText(Canvas& canvas, int row, int& col, Style const& ghostStyle) const
{
    auto const width = canvas.width();
    auto segmenter = unicode::utf8_grapheme_segmenter(_ghostText);
    auto next = segmenter.begin();
    while (next != segmenter.end() && col < width)
    {
        auto const it = next;
        ++next;
        char const* clusterStart = it._clusterStart;
        char const* clusterEnd =
            (next != segmenter.end()) ? next._clusterStart : (_ghostText.data() + _ghostText.size());

        auto style = ghostStyle;
        if (_textDecorator)
        {
            if (auto bg = _textDecorator->background(col))
                style.bg = *bg;
        }

        auto const clusterView =
            std::string_view(clusterStart, static_cast<std::size_t>(clusterEnd - clusterStart));
        col += canvas.putString(row, col, clusterView, style);
    }
}

auto InputField::findLineStart(std::size_t pos) const -> std::size_t
{
    if (pos == 0)
        return 0;

    // Search backward for newline
    std::size_t i = pos;
    while (i > 0)
    {
        --i;
        if (_buffer[i] == '\n')
            return i + 1;
    }
    return 0;
}

auto InputField::findLineEnd(std::size_t pos) const -> std::size_t
{
    std::size_t i = pos;
    while (i < _buffer.size() && _buffer[i] != '\n')
        ++i;
    return i;
}

auto InputField::countGraphemesInRange(std::size_t start, std::size_t end) const -> int
{
    if (start >= end || start >= _buffer.size())
        return 0;

    auto const sv = std::string_view(_buffer).substr(start, end - start);
    auto segmenter = unicode::utf8_grapheme_segmenter(sv);
    int count = 0;
    for ([[maybe_unused]] auto const& cluster: segmenter)
        ++count;
    return count;
}

auto InputField::moveToGraphemeInLine(std::size_t lineStart, int graphemeIndex) const -> std::size_t
{
    auto const lineEnd = findLineEnd(lineStart);
    if (lineStart >= lineEnd)
        return lineStart;

    auto const sv = std::string_view(_buffer).substr(lineStart, lineEnd - lineStart);
    auto segmenter = unicode::utf8_grapheme_segmenter(sv);

    int currentIndex = 0;
    auto it = segmenter.begin();
    while (it != segmenter.end())
    {
        if (currentIndex >= graphemeIndex)
        {
            auto const offset = static_cast<std::size_t>(it._clusterStart - sv.data());
            return lineStart + offset;
        }
        ++currentIndex;
        ++it;
    }

    // Grapheme index exceeds line length, return end of line
    return lineEnd;
}

void InputField::moveToBufferStart()
{
    _cursor = 0;
}

void InputField::moveToBufferEnd()
{
    _cursor = _buffer.size();
}

void InputField::moveToLineStart()
{
    _cursor = findLineStart(_cursor);
}

void InputField::moveToLineEnd()
{
    _cursor = findLineEnd(_cursor);
}

void InputField::smartMoveToLineStart()
{
    if (!_multiline)
    {
        _cursor = 0;
        return;
    }
    auto const lineStart = findLineStart(_cursor);
    if (_cursor != lineStart)
    {
        _cursor = lineStart;
    }
    else if (lineStart > 0)
    {
        // Already at line start, jump to start of previous line
        _cursor = findLineStart(lineStart - 1);
    }
}

void InputField::smartMoveToLineEnd()
{
    if (!_multiline)
    {
        _cursor = _buffer.size();
        return;
    }
    auto const lineEnd = findLineEnd(_cursor);
    if (_cursor != lineEnd)
    {
        _cursor = lineEnd;
    }
    else if (lineEnd < _buffer.size())
    {
        // Already at line end, jump to end of next line
        _cursor = findLineEnd(lineEnd + 1);
    }
}

void InputField::moveUp()
{
    auto const currentLineNum = cursorLine();
    if (currentLineNum == 0)
    {
        // Already on first line, delegate to history
        historyPrev();
        return;
    }

    // Remember current column (in graphemes)
    auto const column = cursorColumn();

    // Find start of current line, then go back to previous line
    auto const currentLineStart = findLineStart(_cursor);
    auto const prevLineEnd = currentLineStart > 0 ? currentLineStart - 1 : 0;
    auto const prevLineStart = findLineStart(prevLineEnd);

    // Move to same column in previous line (or end if line is shorter)
    _cursor = moveToGraphemeInLine(prevLineStart, column);
}

void InputField::moveDown()
{
    auto const currentLineNum = cursorLine();
    auto const totalLines = lineCount();
    if (currentLineNum >= totalLines - 1)
    {
        // Already on last line, delegate to history
        historyNext();
        return;
    }

    // Remember current column (in graphemes)
    auto const column = cursorColumn();

    // Find end of current line, then go to next line
    auto const currentLineEnd = findLineEnd(_cursor);
    auto const nextLineStart = currentLineEnd < _buffer.size() ? currentLineEnd + 1 : _buffer.size();

    // Move to same column in next line (or end if line is shorter)
    _cursor = moveToGraphemeInLine(nextLineStart, column);
}

void InputField::insertNewline()
{
    if (!_multiline)
        return;

    // Check max lines limit
    if (_maxLines > 0 && lineCount() >= _maxLines)
        return;

    _buffer.insert(_cursor, 1, '\n');
    ++_cursor;
}

void InputField::setCursorFromClick(int line, int column, bool extendSelection)
{
    // Clamp line to valid range
    auto const totalLines = lineCount();
    line = std::max(line, 0);
    if (line >= totalLines)
        line = totalLines - 1;

    // Find the start of the target line
    std::size_t lineStart = 0;
    int currentLine = 0;
    while (currentLine < line && lineStart < _buffer.size())
    {
        if (_buffer[lineStart] == '\n')
            ++currentLine;
        ++lineStart;
    }

    // Clamp column to valid range within the line
    column = std::max(column, 0);

    // Move to the target grapheme position within the line
    auto newCursor = moveToGraphemeInLine(lineStart, column);

    // Handle selection
    if (extendSelection)
        startOrExtendSelection();
    else
        clearSelection();

    _cursor = newCursor;
}

// ============================================================================
// Selection support (GUI-style)
// ============================================================================

auto InputField::hasSelection() const noexcept -> bool
{
    return _selectionAnchor.has_value() && *_selectionAnchor != _cursor;
}

auto InputField::selectionStart() const noexcept -> std::size_t
{
    if (!_selectionAnchor)
        return _cursor;
    return std::min(*_selectionAnchor, _cursor);
}

auto InputField::selectionEnd() const noexcept -> std::size_t
{
    if (!_selectionAnchor)
        return _cursor;
    return std::max(*_selectionAnchor, _cursor);
}

auto InputField::selectedText() const -> std::string_view
{
    if (!hasSelection())
        return {};
    return std::string_view(_buffer).substr(selectionStart(), selectionEnd() - selectionStart());
}

void InputField::clearSelection()
{
    _selectionAnchor.reset();
}

void InputField::selectAll()
{
    _selectionAnchor = 0;
    _cursor = _buffer.size();
}

void InputField::setClipboardCallback(ClipboardCallback callback)
{
    _clipboardCallback = std::move(callback);
}

auto InputField::copySelection() -> bool
{
    if (!hasSelection())
        return false;
    auto const text = selectedText();
    pushKillRing(std::string(text));
    // Also copy to system clipboard if callback is set
    if (_clipboardCallback)
        _clipboardCallback(text);
    return true;
}

auto InputField::cutSelection() -> bool
{
    if (!hasSelection())
        return false;
    auto const text = selectedText();
    pushKillRing(std::string(text));
    // Also copy to system clipboard if callback is set
    if (_clipboardCallback)
        _clipboardCallback(text);
    deleteSelection();
    return true;
}

void InputField::deleteSelection()
{
    if (!hasSelection())
        return;
    auto const start = selectionStart();
    auto const end = selectionEnd();
    _buffer.erase(start, end - start);
    _cursor = start;
    clearSelection();
}

void InputField::startOrExtendSelection()
{
    if (!_selectionAnchor)
        _selectionAnchor = _cursor;
}

void InputField::moveWithSelection(void (InputField::*move)())
{
    startOrExtendSelection();
    (this->*move)();
}

// ============================================================================
// Undo/Redo support
// ============================================================================

void InputField::saveUndoState()
{
    // Clear redo stack when new edit happens
    _redoStack.clear();

    // Save current state
    _undoStack.push_back(UndoState { .buffer = _buffer, .cursor = _cursor });

    // Limit undo history size
    if (_undoStack.size() > MaxUndoHistory)
        _undoStack.erase(_undoStack.begin());
}

auto InputField::undo() -> bool
{
    if (_undoStack.empty())
        return false;

    // Save current state to redo stack
    _redoStack.push_back(UndoState { .buffer = _buffer, .cursor = _cursor });

    // Restore previous state
    auto const& state = _undoStack.back();
    _buffer = state.buffer;
    _cursor = state.cursor;
    _undoStack.pop_back();

    clearSelection();
    return true;
}

auto InputField::redo() -> bool
{
    if (_redoStack.empty())
        return false;

    // Save current state to undo stack
    _undoStack.push_back(UndoState { .buffer = _buffer, .cursor = _cursor });

    // Restore redo state
    auto const& state = _redoStack.back();
    _buffer = state.buffer;
    _cursor = state.cursor;
    _redoStack.pop_back();

    clearSelection();
    return true;
}

auto InputField::canUndo() const noexcept -> bool
{
    return !_undoStack.empty();
}

auto InputField::canRedo() const noexcept -> bool
{
    return !_redoStack.empty();
}

void InputField::clearUndoHistory()
{
    _undoStack.clear();
    _redoStack.clear();
}

// ============================================================================
// Mouse handling
// ============================================================================

auto InputField::handleMouse(MouseEvent::Type type, int line, int column, Modifier mods) -> InputFieldAction
{
    bool const shift = (mods & Modifier::Shift) != Modifier::None;

    switch (type)
    {
        case MouseEvent::Type::Press: {
            // Only handle left button for now (button 0)
            int const clickCount = detectClickCount(line, column);

            if (clickCount == 1)
            {
                // Single click: position cursor, optionally extend selection
                _dragging = true;
                setCursorFromClick(line, column, shift);
            }
            else if (clickCount == 2)
            {
                // Double click: select word
                _dragging = false;
                setCursorFromClick(line, column, false);
                selectWord(_cursor);
            }
            else // clickCount >= 3
            {
                // Triple click: select line
                _dragging = false;
                selectLine(line);
            }
            return InputFieldAction::Changed;
        }

        case MouseEvent::Type::Move:
            if (_dragging)
            {
                // Extend selection while dragging
                setCursorFromClick(line, column, true);
                return InputFieldAction::Changed;
            }
            break;

        case MouseEvent::Type::Release: _dragging = false; break;

        case MouseEvent::Type::ScrollUp:
            if (_multiline && lineCount() > 1)
            {
                scrollBy(-3);
                return InputFieldAction::Changed;
            }
            break;

        case MouseEvent::Type::ScrollDown:
            if (_multiline && lineCount() > 1)
            {
                scrollBy(3);
                return InputFieldAction::Changed;
            }
            break;
    }

    return InputFieldAction::None;
}

auto InputField::detectClickCount(int line, int column) -> int
{
    auto const now = _clock.now();
    auto const elapsed = now - _lastClickTime;

    // Check if this is a continuation of multi-click sequence
    bool const withinTimeout = elapsed < DoubleClickTimeout;
    bool const nearLastClick =
        std::abs(line - _lastClickLine) <= 0 && std::abs(column - _lastClickColumn) <= DoubleClickTolerance;

    if (withinTimeout && nearLastClick)
    {
        _clickCount = std::min(_clickCount + 1, 3);
    }
    else
    {
        _clickCount = 1;
    }

    _lastClickTime = now;
    _lastClickLine = line;
    _lastClickColumn = column;

    return _clickCount;
}

auto InputField::findWordStart(std::size_t pos) const -> std::size_t
{
    if (pos == 0 || _buffer.empty())
        return 0;

    // Move to start of current character
    std::size_t current = pos;
    current = std::min(current, _buffer.size());
    if (current > 0)
        current = prevGraphemeCluster(current);

    // If we're on a non-word char, just return current position
    if (!isWordChar(decodeUtf8At(_buffer, current)))
        return pos;

    // Move backward while on word characters
    while (current > 0)
    {
        std::size_t const prev = prevGraphemeCluster(current);
        if (!isWordChar(decodeUtf8At(_buffer, prev)))
            break;
        current = prev;
    }

    return current;
}

auto InputField::findWordEnd(std::size_t pos) const -> std::size_t
{
    if (_buffer.empty())
        return 0;

    std::size_t current = pos;
    if (current >= _buffer.size())
        return _buffer.size();

    // If we're on a non-word char, just return current position
    if (!isWordChar(decodeUtf8At(_buffer, current)))
        return pos;

    // Move forward while on word characters
    while (current < _buffer.size() && isWordChar(decodeUtf8At(_buffer, current)))
    {
        current = nextGraphemeCluster(current);
    }

    return current;
}

void InputField::selectWord(std::size_t position)
{
    if (_buffer.empty())
        return;

    // Clamp position to buffer bounds
    if (position >= _buffer.size())
        position = !_buffer.empty() ? _buffer.size() - 1 : 0;

    // If on a non-word character, don't select anything meaningful
    if (!isWordChar(decodeUtf8At(_buffer, position)))
    {
        // Select just this character
        _selectionAnchor = position;
        _cursor = nextGraphemeCluster(position);
        return;
    }

    std::size_t wordStart = findWordStart(position);
    std::size_t const wordEnd = findWordEnd(position);

    _selectionAnchor = wordStart;
    _cursor = wordEnd;
}

void InputField::selectLine(int lineIndex)
{
    auto const totalLines = lineCount();
    lineIndex = std::max(lineIndex, 0);
    if (lineIndex >= totalLines)
        lineIndex = totalLines - 1;

    // Find line boundaries
    std::size_t lineStart = 0;
    int currentLine = 0;
    while (currentLine < lineIndex && lineStart < _buffer.size())
    {
        if (_buffer[lineStart] == '\n')
            ++currentLine;
        ++lineStart;
    }

    std::size_t lineEnd = lineStart;
    while (lineEnd < _buffer.size() && _buffer[lineEnd] != '\n')
        ++lineEnd;

    // Include the newline in selection if not at end of buffer
    if (lineEnd < _buffer.size() && _buffer[lineEnd] == '\n')
        ++lineEnd;

    _selectionAnchor = lineStart;
    _cursor = lineEnd;
}

void InputField::scrollBy(int lines)
{
    auto const totalLines = lineCount();
    auto const maxOffset = std::max(0, totalLines - 1);

    _scrollOffset = std::clamp(_scrollOffset + lines, 0, maxOffset);
}

auto InputField::scrollOffset() const noexcept -> int
{
    return _scrollOffset;
}

void InputField::setScrollOffset(int offset)
{
    auto const totalLines = lineCount();
    auto const maxOffset = std::max(0, totalLines - 1);
    _scrollOffset = std::clamp(offset, 0, maxOffset);
}

} // namespace core::tui
