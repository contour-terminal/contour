// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace core::tui
{

class TerminalOutput;

/// @brief Abstract base class for custom renderable elements.
///
/// Subclasses implement render() to draw themselves using a TerminalOutput.
/// This provides a lightweight widget abstraction for composing TUI layouts.
class Element
{
  public:
    virtual ~Element() = default;

    /// @brief Renders this element to the terminal output at the current cursor position.
    /// @param output The terminal output to render to.
    virtual void render(TerminalOutput& output) = 0;

    /// @brief Minimum width in columns. Returns 0 if flexible.
    [[nodiscard]] virtual auto minWidth() const -> int { return 0; }

    /// @brief Preferred height in rows. Returns 1 by default.
    [[nodiscard]] virtual auto preferredHeight() const -> int { return 1; }
};

} // namespace core::tui
