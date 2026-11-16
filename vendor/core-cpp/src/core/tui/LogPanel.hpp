// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace core::tui
{

/// @brief Severity level for log entries.
enum class LogLevel : std::uint8_t
{
    Info,
    Warning,
    Error,
};

/// @brief A single log entry with level and message text.
struct LogEntry
{
    LogLevel level;      ///< Severity of the log entry.
    std::string message; ///< The log message text.
};

/// @brief Collapsible log panel component rendered at the bottom of the TUI.
///
/// Supports toggling between collapsed (1 header row) and expanded (header + entries)
/// states. In expanded state, supports scrolling through entries via mouse scroll
/// or keyboard navigation.
class LogPanel
{
  public:
    /// @brief Adds a log entry to the panel.
    /// @param level The severity level.
    /// @param message The log message text.
    void addLog(LogLevel level, std::string message);

    /// @brief Toggles between expanded and collapsed state.
    void toggle();

    /// @brief Returns whether the panel is currently expanded.
    [[nodiscard]] auto isExpanded() const noexcept -> bool;

    /// @brief Returns the total number of log entries.
    [[nodiscard]] auto entryCount() const noexcept -> std::size_t;

    /// @brief Returns the total height in rows (1 if collapsed, 1 + visible entries if expanded).
    [[nodiscard]] auto totalHeight() const noexcept -> int;

    /// @brief Renders the log panel starting at the given row, spanning the full terminal width.
    /// @param output The terminal output to render to.
    /// @param startRow The 1-based row where the panel header is drawn.
    /// @param cols The terminal width in columns.
    void render(TerminalOutput& output, int startRow, int cols);

    /// @brief Handles a mouse click event, checking if it hits the toggle symbol or triggers scroll.
    /// @param x Column of the click (1-based).
    /// @param y Row of the click (1-based).
    /// @param panelStartRow The 1-based row where the panel header is drawn.
    /// @return True if the click was handled by the panel.
    [[nodiscard]] auto handleClick(int x, int y, int panelStartRow) -> bool;

    /// @brief Scrolls the visible entries up (towards older entries).
    void scrollUp();

    /// @brief Scrolls the visible entries down (towards newer entries).
    void scrollDown();

    /// @brief Maximum number of entries retained in the log.
    static constexpr int MaxEntries = 100;

    /// @brief Maximum visible entries when expanded.
    static constexpr int MaxVisibleExpanded = 7;

  private:
    mutable std::mutex _mutex;
    std::deque<LogEntry> _entries;
    bool _expanded = false;
    int _scrollOffset = 0; ///< Scroll offset from the newest entry (0 = bottom, showing newest).
};

} // namespace core::tui
