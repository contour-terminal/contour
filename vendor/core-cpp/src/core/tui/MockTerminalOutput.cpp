// SPDX-License-Identifier: Apache-2.0
#include <core/tui/MockTerminalOutput.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace core::tui
{

MockTerminalOutput::MockTerminalOutput(int cols, int rows): _cols(cols), _rows(rows)
{
}

auto MockTerminalOutput::initialize() -> VoidResult
{
    return {};
}

void MockTerminalOutput::writeText(std::string_view text, [[maybe_unused]] Style const& style)
{
    // Accumulate into the open hyperlink run, if any. Only text actually written counts, so a
    // frame whose diff skipped cells yields a run holding just the rewritten graphemes —
    // which is precisely what such a test wants to assert about.
    if (_hyperlinkOpen)
        _hyperlinkRuns.back().text += text;

    // Advance cursor column by display width of text.
    // No auto-wrap: real terminals use "deferred wrap" at column boundary,
    // and inline rendering uses \r at each row start to reset the column.
    // Row advancement is handled by \n in writeRaw().
    for (auto const ch: text)
    {
        if ((static_cast<unsigned char>(ch) & 0xC0) == 0x80)
            continue; // Skip UTF-8 continuation bytes
        ++_cursorCol;
    }
}

void MockTerminalOutput::writeRaw(std::string_view text)
{
    _rawWrites.push_back(RawWrite { .row = _cursorRow, .column = _cursorCol, .text = std::string { text } });

    for (auto const ch: text)
    {
        if (ch == '\n')
        {
            ++_cursorRow;
            if (_cursorRow >= _rows)
            {
                _cursorRow = _rows - 1;
                ++_scrollCount;
            }
        }
        else if (ch == '\r')
        {
            _cursorCol = 0;
        }
        else if (ch == '\033')
        {
            // Stop processing — the rest is a VT escape sequence.
            // The mock doesn't parse escape sequences; callers should use
            // the semantic methods (moveUp, clearLine, etc.) instead.
            return;
        }
        else
        {
            if ((static_cast<unsigned char>(ch) & 0xC0) == 0x80)
                continue; // Skip UTF-8 continuation bytes
            // No auto-wrap for printable characters (same as writeText).
            ++_cursorCol;
        }
    }
}

void MockTerminalOutput::moveTo(int row, int col)
{
    // moveTo uses 1-based coordinates; convert to 0-based.
    _cursorRow = std::clamp(row - 1, 0, _rows - 1);
    _cursorCol = std::clamp(col - 1, 0, _cols - 1);
}

void MockTerminalOutput::moveUp(int n)
{
    _cursorRow = std::max(0, _cursorRow - n);
}

void MockTerminalOutput::moveDown(int n)
{
    _cursorRow = std::min(_rows - 1, _cursorRow + n);
}

void MockTerminalOutput::moveLeft(int n)
{
    _cursorCol = std::max(0, _cursorCol - n);
}

void MockTerminalOutput::moveRight(int n)
{
    _cursorCol = std::min(_cols - 1, _cursorCol + n);
}

void MockTerminalOutput::carriageReturn()
{
    _cursorCol = 0;
}

void MockTerminalOutput::linefeed()
{
    ++_cursorRow;
    if (_cursorRow >= _rows)
    {
        _cursorRow = _rows - 1;
        ++_scrollCount;
    }
}

void MockTerminalOutput::clearToEndOfDisplay()
{
    // No-op on mock canvas — no pixel/character buffer to clear.
}

void MockTerminalOutput::requestCellSize()
{
    // No-op — mock does not respond to terminal queries.
}

void MockTerminalOutput::requestCursorPosition()
{
    // No-op — mock does not respond to terminal queries.
}

void MockTerminalOutput::requestDecMode(int /*mode*/)
{
    // No-op — mock does not respond to terminal queries.
}

void MockTerminalOutput::requestDeviceAttributes()
{
    // No-op — mock does not respond to terminal queries.
}

void MockTerminalOutput::clearLine()
{
    // No-op on mock canvas — just semantic tracking.
}

void MockTerminalOutput::clearToEndOfLine()
{
    // No-op on mock canvas.
}

void MockTerminalOutput::clearToStartOfLine()
{
    // No-op on mock canvas.
}

void MockTerminalOutput::deleteLines(int n)
{
    // The mock holds no cell content, so only the count is tracked; the cursor does not move.
    if (n > 0)
        _deletedLines += n;
}

void MockTerminalOutput::clearScreen()
{
    _cursorRow = 0;
    _cursorCol = 0;
}

void MockTerminalOutput::clearScrollback()
{
    // No-op.
}

void MockTerminalOutput::enterAltScreen()
{
    // No-op.
}

void MockTerminalOutput::leaveAltScreen()
{
    // No-op.
}

auto MockTerminalOutput::syncGuard() -> SyncGuard
{
    return {}; // No-op guard (handle == -1).
}

void MockTerminalOutput::setDoubleWidth()
{
    // No-op.
}

void MockTerminalOutput::setDoubleHeightTop()
{
    // No-op.
}

void MockTerminalOutput::setDoubleHeightBottom()
{
    // No-op.
}

void MockTerminalOutput::setSingleWidth()
{
    // No-op.
}

void MockTerminalOutput::disableReflow()
{
    // No-op.
}

void MockTerminalOutput::enableReflow()
{
    // No-op.
}

void MockTerminalOutput::showCursor()
{
    _cursorVisible = true;
}

void MockTerminalOutput::hideCursor()
{
    _cursorVisible = false;
}

void MockTerminalOutput::saveCursor()
{
    _savedCursorRow = _cursorRow;
    _savedCursorCol = _cursorCol;
}

void MockTerminalOutput::restoreCursor()
{
    _cursorRow = _savedCursorRow;
    _cursorCol = _savedCursorCol;
}

void MockTerminalOutput::setCursorShape([[maybe_unused]] CursorShape shape)
{
    // No-op.
}

void MockTerminalOutput::setScrollRegion([[maybe_unused]] int top, [[maybe_unused]] int bottom)
{
    // No-op.
}

void MockTerminalOutput::resetScrollRegion()
{
    // No-op.
}

void MockTerminalOutput::writeSixel([[maybe_unused]] std::string_view sixelData)
{
    // No-op.
}

void MockTerminalOutput::copyToClipboard(std::string_view text)
{
    _clipboard.assign(text);
}

std::string const& MockTerminalOutput::clipboardText() const noexcept
{
    return _clipboard;
}

void MockTerminalOutput::unscroll([[maybe_unused]] int n)
{
    // No-op.
}

bool MockTerminalOutput::supportsUnscroll() const noexcept
{
    return false;
}

void MockTerminalOutput::beginHyperlink(std::string_view url, std::string_view id)
{
    // Framing occupies no cells, so the cursor does not move; the run is recorded so tests
    // can assert on logical links rather than on raw escape bytes.
    _hyperlinkRuns.push_back(HyperlinkRun {
        .url = std::string { url },
        .id = std::string { id },
        .text = {},
    });
    _hyperlinkOpen = true;
}

void MockTerminalOutput::endHyperlink()
{
    if (!_hyperlinkOpen)
    {
        ++_unbalancedHyperlinkCloses;
        return;
    }
    _hyperlinkOpen = false;
}

void MockTerminalOutput::writeHyperlink(std::string_view text, std::string_view url, Style const& style)
{
    beginHyperlink(url, {});
    writeText(text, style);
    endHyperlink();
}

void MockTerminalOutput::flush()
{
    ++_flushCount;
}

auto MockTerminalOutput::columns() const noexcept -> int
{
    return _cols;
}

auto MockTerminalOutput::rows() const noexcept -> int
{
    return _rows;
}

void MockTerminalOutput::updateDimensions()
{
    // No-op — dimensions are fixed for the mock.
}

int MockTerminalOutput::cursorRow() const noexcept
{
    return _cursorRow;
}

int MockTerminalOutput::cursorCol() const noexcept
{
    return _cursorCol;
}

auto MockTerminalOutput::kind() const noexcept -> OutputKind
{
    return OutputKind::Simulation;
}

auto MockTerminalOutput::simulatedCursorPosition() const noexcept -> std::optional<std::pair<int, int>>
{
    return std::pair { _cursorRow + 1, _cursorCol + 1 };
}

bool MockTerminalOutput::cursorVisible() const noexcept
{
    return _cursorVisible;
}

int MockTerminalOutput::scrollCount() const noexcept
{
    return _scrollCount;
}

int MockTerminalOutput::flushCount() const noexcept
{
    return _flushCount;
}

auto MockTerminalOutput::hyperlinkRuns() const noexcept -> std::span<HyperlinkRun const>
{
    return _hyperlinkRuns;
}

bool MockTerminalOutput::hyperlinkOpen() const noexcept
{
    return _hyperlinkOpen;
}

int MockTerminalOutput::unbalancedHyperlinkCloses() const noexcept
{
    return _unbalancedHyperlinkCloses;
}

auto MockTerminalOutput::rawWrites() const noexcept -> std::span<RawWrite const>
{
    return _rawWrites;
}

int MockTerminalOutput::deletedLines() const noexcept
{
    return _deletedLines;
}

} // namespace core::tui
