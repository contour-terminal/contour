// SPDX-License-Identifier: Apache-2.0
#include <core/tui/LogPanel.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <utility>

namespace core::tui
{

void LogPanel::addLog(LogLevel level, std::string message)
{
    auto const lock = std::scoped_lock(_mutex);
    _entries.push_back(LogEntry { .level = level, .message = std::move(message) });
    if (static_cast<int>(_entries.size()) > MaxEntries)
        _entries.pop_front();

    // Reset scroll to bottom when new entries arrive
    _scrollOffset = 0;
}

void LogPanel::toggle()
{
    auto const lock = std::scoped_lock(_mutex);
    _expanded = !_expanded;
    if (_expanded)
        _scrollOffset = 0; // Reset scroll when expanding
}

auto LogPanel::isExpanded() const noexcept -> bool
{
    auto const lock = std::scoped_lock(_mutex);
    return _expanded;
}

auto LogPanel::entryCount() const noexcept -> std::size_t
{
    auto const lock = std::scoped_lock(_mutex);
    return _entries.size();
}

auto LogPanel::totalHeight() const noexcept -> int
{
    auto const lock = std::scoped_lock(_mutex);
    if (!_expanded)
        return 1; // Just the header row

    auto const visibleCount = std::min(static_cast<int>(_entries.size()), MaxVisibleExpanded);
    return 1 + std::max(visibleCount, 1); // Header + at least 1 row (empty or entries)
}

void LogPanel::render(TerminalOutput& output, int startRow, int cols)
{
    auto const lock = std::scoped_lock(_mutex);
    auto const entrySize = static_cast<int>(_entries.size());

    // Header row: separator line with toggle symbol
    output.moveTo(startRow, 1);
    output.clearLine();

    auto const* const toggleSymbol = _expanded ? "\u25BC" : "\u25B6"; // ▼ or ▶
    auto const headerText = std::format("{} Logs ({})", toggleSymbol, entrySize);

    // Draw separator with embedded header
    auto headerStyle = Style {};
    headerStyle.fg = static_cast<std::uint8_t>(8); // Gray
    headerStyle.bold = true;

    auto separatorStyle = Style {};
    separatorStyle.fg = static_cast<std::uint8_t>(8); // Gray

    // Draw: ── ▶ Logs (N) ───────────
    auto const leftBar = 2;
    auto headerPrefix = std::string {};
    for ([[maybe_unused]] auto const i: std::views::iota(0, std::max(0, leftBar)))
        headerPrefix += "\u2500"; // ─
    headerPrefix += " ";

    output.writeText(headerPrefix, separatorStyle);
    output.writeText(headerText, headerStyle);
    output.writeText(" ", separatorStyle);

    // Fill rest with ─
    auto const usedCols = leftBar + 1 + static_cast<int>(headerText.size()) + 1;
    auto const remainingCols = std::max(0, cols - usedCols);
    auto fillStr = std::string {};
    for ([[maybe_unused]] auto const i: std::views::iota(0, std::max(0, remainingCols)))
        fillStr += "\u2500"; // ─
    output.writeText(fillStr, separatorStyle);

    if (!_expanded)
        return;

    // Render visible entries (newest first, with scroll offset)
    auto const visibleCount = std::min(entrySize, MaxVisibleExpanded);
    auto const maxScrollable = std::max(0, entrySize - MaxVisibleExpanded);
    auto const clampedOffset = std::clamp(_scrollOffset, 0, maxScrollable);

    // Entries are stored oldest-first. We show newest-first.
    // With scrollOffset=0, we show the last `visibleCount` entries (newest).
    // With scrollOffset=N, we skip N entries from the end.
    auto const endIdx = entrySize - clampedOffset;
    auto const startIdx = std::max(0, endIdx - visibleCount);

    for (auto const row: std::views::iota(0, std::max({ 0, visibleCount, 1 })))
    {
        auto const entryIdx = startIdx + row;
        output.moveTo(startRow + 1 + row, 1);
        output.clearLine();

        if (entryIdx < endIdx && entryIdx < entrySize)
        {
            auto const& entry = _entries[static_cast<std::size_t>(entryIdx)];

            // Level tag
            auto levelStyle = Style {};
            auto levelTag = std::string {};
            switch (entry.level)
            {
                case LogLevel::Info:
                    levelStyle.fg = static_cast<std::uint8_t>(4); // Blue
                    levelTag = "[Info]";
                    break;
                case LogLevel::Warning:
                    levelStyle.fg = static_cast<std::uint8_t>(3); // Yellow
                    levelTag = "[Warn]";
                    break;
                case LogLevel::Error:
                    levelStyle.fg = static_cast<std::uint8_t>(1); // Red
                    levelTag = "[Error]";
                    break;
            }
            levelStyle.bold = true;
            output.writeText(std::format(" {} ", levelTag), levelStyle);

            // Message text (truncate to fit)
            auto const prefixLen = static_cast<int>(levelTag.size()) + 3; // " [Tag] "
            auto const maxMsgLen = std::max(0, cols - prefixLen);
            auto const& msg = entry.message;
            if (std::cmp_greater(msg.size(), maxMsgLen))
                output.writeRaw(msg.substr(0, static_cast<std::size_t>(maxMsgLen)));
            else
                output.writeRaw(msg);
        }
    }

    // Show scroll indicator if there are more entries
    if (maxScrollable > 0)
    {
        auto indicatorStyle = Style {};
        indicatorStyle.fg = static_cast<std::uint8_t>(8); // Gray
        indicatorStyle.dim = true;

        auto const lastRow = startRow + std::max(visibleCount, 1);
        auto const indicator =
            std::format(" [{}-{}/{}]", startIdx + 1, std::min(endIdx, entrySize), entrySize);
        auto const indicatorCol = std::max(1, cols - static_cast<int>(indicator.size()));
        output.moveTo(lastRow, indicatorCol);
        output.writeText(indicator, indicatorStyle);
    }
}

auto LogPanel::handleClick(int /*x*/, int y, int panelStartRow) -> bool
{
    // Click on the header row toggles the panel
    if (y == panelStartRow)
    {
        auto const lock = std::scoped_lock(_mutex);
        _expanded = !_expanded;
        if (_expanded)
            _scrollOffset = 0;
        return true;
    }
    return false;
}

void LogPanel::scrollUp()
{
    auto const lock = std::scoped_lock(_mutex);
    auto const maxScrollable = std::max(0, static_cast<int>(_entries.size()) - MaxVisibleExpanded);
    _scrollOffset = std::min(_scrollOffset + 1, maxScrollable);
}

void LogPanel::scrollDown()
{
    auto const lock = std::scoped_lock(_mutex);
    _scrollOffset = std::max(0, _scrollOffset - 1);
}

} // namespace core::tui
