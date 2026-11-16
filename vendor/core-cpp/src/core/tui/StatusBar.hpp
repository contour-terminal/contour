// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Component.hpp>
#include <core/tui/Theme.hpp>

#include <string>
#include <vector>

namespace core::tui
{

/// @brief A keyboard shortcut hint displayed in the status bar.
struct KeyHint
{
    std::string key;    ///< The key combination (e.g., "Ctrl+C").
    std::string action; ///< The action description (e.g., "Quit").
};

/// @brief Configuration for StatusBar styling.
struct StatusBarStyle
{
    Style background;                      ///< Background style for the entire bar.
    Style keyStyle;                        ///< Style for the key part of hints.
    Style actionStyle;                     ///< Style for the action description.
    Style separator;                       ///< Style for separators between hints.
    std::string_view separatorChar = "  "; ///< Character(s) between hints.
};

/// @brief A status bar component for displaying keyboard shortcuts and status info.
///
/// Typically rendered at the bottom of the screen. Supports left, center, and right
/// aligned sections for different types of information.
/// As a Component, StatusBar can be added to a Screen's component tree.
class StatusBar: public Component
{
  public:
    /// @brief Constructs an empty status bar.
    StatusBar() = default;
    ~StatusBar() override = default;

    // --- Component Interface ---

    /// @brief Renders the status bar to the canvas.
    void render(Canvas& canvas) override;

    /// @brief StatusBar doesn't handle events by default.
    [[nodiscard]] EventResult onEvent([[maybe_unused]] InputEvent const& event) override
    {
        return EventResult::Ignored;
    }

    /// @brief StatusBar is not focusable by default.
    [[nodiscard]] bool focusable() const override { return false; }

    /// @brief Returns preferred size (1 row, full width).
    [[nodiscard]] Size preferredSize() const override;

    /// @brief Sets the keyboard hints to display.
    /// @param hints The hints to display.
    void setHints(std::vector<KeyHint> hints);

    /// @brief Adds a single hint.
    /// @param key The key combination.
    /// @param action The action description.
    void addHint(std::string key, std::string action);

    /// @brief Clears all hints.
    void clearHints();

    /// @brief Sets text for the left section.
    /// @param text The text to display.
    void setLeftText(std::string text);

    /// @brief Sets text for the center section.
    /// @param text The text to display.
    void setCenterText(std::string text);

    /// @brief Sets text for the right section.
    /// @param text The text to display.
    void setRightText(std::string text);

    /// @brief Sets the style configuration.
    /// @param style The style configuration.
    void setStyle(StatusBarStyle style);

    /// @brief Returns the current style configuration.
    [[nodiscard]] auto style() const noexcept -> StatusBarStyle const&;

  private:
    std::vector<KeyHint> _hints;
    std::string _leftText;
    std::string _centerText;
    std::string _rightText;
    StatusBarStyle _style;

    [[nodiscard]] auto formatHints() const -> std::string;
    [[nodiscard]] auto hintsWidth() const -> int;
};

/// @brief Returns a default status bar style.
[[nodiscard]] auto defaultStatusBarStyle() -> StatusBarStyle;

} // namespace core::tui
