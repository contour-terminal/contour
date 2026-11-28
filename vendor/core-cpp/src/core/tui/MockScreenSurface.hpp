// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/ScreenSurface.hpp>
#include <core/tui/TuiCoreExport.hpp>

#include <expected>
#include <memory>
#include <utility>

namespace core::tui
{

/// @brief A @c ScreenSurface over a @c MockTerminalOutput, for testing a @c Screen with no terminal.
///
/// Its size is the mock's, and it answers the inline viewport's cursor query from the mock's own
/// cursor -- what a @c Terminal over a mock output answers, without the terminal half of the TUI.
class CORE_TUI_CORE_API MockScreenSurface final: public ScreenSurface
{
  public:
    /// @brief Constructs a surface over an 80x24 mock output.
    MockScreenSurface();

    /// @brief Constructs a surface over @p output.
    /// @param output The mock every frame is written to (ownership transferred; must not be null).
    explicit MockScreenSurface(std::unique_ptr<MockTerminalOutput> output);

    [[nodiscard]] auto columns() const noexcept -> int override;
    [[nodiscard]] auto rows() const noexcept -> int override;
    [[nodiscard]] auto output() noexcept -> TerminalOutput& override;

    /// @brief Answers from the mock's cursor.
    /// @return The mock's cursor as (row, column), both 1-based; or @c QueryUnanswered::NotAsked when
    ///         the output, a subclass of the mock, reports no simulated cursor.
    [[nodiscard]] auto queryCursorPosition() -> std::expected<std::pair<int, int>, QueryUnanswered> override;

    /// @brief Returns the mock the frames are written to.
    [[nodiscard]] auto mock() noexcept -> MockTerminalOutput& { return *_output; }

  private:
    std::unique_ptr<MockTerminalOutput> _output;
};

} // namespace core::tui
