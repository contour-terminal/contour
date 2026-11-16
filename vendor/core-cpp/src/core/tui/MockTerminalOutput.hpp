// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp>

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace core::tui
{

/// @brief A run of text written while one OSC 8 hyperlink was open.
struct HyperlinkRun
{
    std::string url;  ///< URL passed to beginHyperlink().
    std::string id;   ///< OSC 8 `id=` value (empty when none was given).
    std::string text; ///< Concatenated writeText() payloads while the link was open.
};

/// @brief Mock terminal output for testing.
///
/// Tracks cursor position and visibility semantically instead of writing VT sequences.
/// Use this to verify cursor positioning logic in tests without a real terminal.
class MockTerminalOutput: public TerminalOutput
{
  public:
    /// @brief One writeRaw() call, with the cursor position it was made at.
    struct RawWrite
    {
        int row;          ///< Cursor row (0-based) when the write began.
        int column;       ///< Cursor column (0-based) when the write began.
        std::string text; ///< The bytes passed to writeRaw().
    };

    /// @brief Constructs a mock terminal output with the given dimensions.
    /// @param cols Number of columns.
    /// @param rows Number of rows.
    explicit MockTerminalOutput(int cols = 80, int rows = 24);

    // --- Virtual overrides ---

    [[nodiscard]] auto initialize() -> VoidResult override;
    void writeText(std::string_view text, Style const& style) override;
    void writeRaw(std::string_view text) override;
    void moveTo(int row, int col) override;
    void moveUp(int n) override;
    void moveDown(int n) override;
    void moveLeft(int n) override;
    void moveRight(int n) override;
    void carriageReturn() override;
    void linefeed() override;
    void clearToEndOfDisplay() override;
    void requestCellSize() override;
    void requestCursorPosition() override;
    void requestDecMode(int mode) override;
    void requestDeviceAttributes() override;
    void clearLine() override;
    void clearToEndOfLine() override;
    void clearToStartOfLine() override;
    void deleteLines(int n) override;
    void clearScreen() override;
    void clearScrollback() override;
    void enterAltScreen() override;
    void leaveAltScreen() override;
    [[nodiscard]] auto syncGuard() -> SyncGuard override;
    void setDoubleWidth() override;
    void setDoubleHeightTop() override;
    void setDoubleHeightBottom() override;
    void setSingleWidth() override;
    void disableReflow() override;
    void enableReflow() override;
    void showCursor() override;
    void hideCursor() override;
    void saveCursor() override;
    void restoreCursor() override;
    void setCursorShape(CursorShape shape) override;
    void setScrollRegion(int top, int bottom) override;
    void resetScrollRegion() override;
    void writeSixel(std::string_view sixelData) override;
    void copyToClipboard(std::string_view text) override;
    void beginHyperlink(std::string_view url, std::string_view id) override;
    void endHyperlink() override;
    void writeHyperlink(std::string_view text, std::string_view url, Style const& style) override;
    void unscroll(int n) override;
    [[nodiscard]] bool supportsUnscroll() const noexcept override;
    void flush() override;
    [[nodiscard]] auto columns() const noexcept -> int override;
    [[nodiscard]] auto rows() const noexcept -> int override;
    void updateDimensions() override;

    /// @brief A mock is a simulation: no terminal reads what it is given.
    /// @return @c OutputKind::Simulation.
    [[nodiscard]] auto kind() const noexcept -> OutputKind override;

    /// @brief Answers from the mock's own cursor.
    /// @return The cursor as (row, column), both 1-based.
    [[nodiscard]] auto simulatedCursorPosition() const noexcept
        -> std::optional<std::pair<int, int>> override;

    // --- Test-specific accessors ---

    /// @brief Returns the current cursor row (0-based).
    [[nodiscard]] int cursorRow() const noexcept;

    /// @brief Returns the current cursor column (0-based).
    [[nodiscard]] int cursorCol() const noexcept;

    /// @brief Returns whether the cursor is currently visible.
    [[nodiscard]] bool cursorVisible() const noexcept;

    /// @brief Returns the total number of scroll operations since construction.
    [[nodiscard]] int scrollCount() const noexcept;

    /// @brief Returns the total number of flush() calls since construction.
    [[nodiscard]] int flushCount() const noexcept;

    /// @brief Returns the text most recently passed to copyToClipboard (empty if never called).
    [[nodiscard]] std::string const& clipboardText() const noexcept;

    /// @brief Returns every hyperlink run recorded since construction, in emission order.
    [[nodiscard]] std::span<HyperlinkRun const> hyperlinkRuns() const noexcept;

    /// @brief Whether a hyperlink is still open, i.e. a flush leaked its framing.
    [[nodiscard]] bool hyperlinkOpen() const noexcept;

    /// @brief Number of endHyperlink() calls with no matching open (framing imbalance).
    [[nodiscard]] int unbalancedHyperlinkCloses() const noexcept;

    /// @brief Returns every writeRaw() call since construction, in order, with its position.
    [[nodiscard]] std::span<RawWrite const> rawWrites() const noexcept;

    /// @brief Returns the total number of lines deleted through deleteLines() since construction.
    [[nodiscard]] int deletedLines() const noexcept;

  private:
    int _cols;
    int _rows;
    int _cursorRow = 0;
    int _cursorCol = 0;
    int _savedCursorRow = 0;
    int _savedCursorCol = 0;
    bool _cursorVisible = true;
    int _scrollCount = 0;
    int _flushCount = 0;
    std::string _clipboard;
    std::vector<HyperlinkRun> _hyperlinkRuns;
    bool _hyperlinkOpen = false;
    int _unbalancedHyperlinkCloses = 0;
    std::vector<RawWrite> _rawWrites;
    int _deletedLines = 0;
};

} // namespace core::tui
