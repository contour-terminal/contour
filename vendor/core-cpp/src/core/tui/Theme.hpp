// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Box.hpp>
#include <core/tui/TerminalOutput.hpp>

namespace core::tui
{

/// @brief Defines a color palette for the TUI theme.
struct ColorPalette
{
    // Primary colors
    RgbColor primary;   ///< Primary accent color.
    RgbColor secondary; ///< Secondary accent color.
    RgbColor accent;    ///< Additional accent color.

    // Background colors
    RgbColor background; ///< Main background.
    RgbColor surface;    ///< Surface/panel background.
    RgbColor overlay;    ///< Overlay/dialog background.

    // Text colors
    RgbColor text;        ///< Primary text color.
    RgbColor textMuted;   ///< Muted/secondary text.
    RgbColor textInverse; ///< Text on accent backgrounds.

    // Semantic colors
    RgbColor success; ///< Success/positive color.
    RgbColor warning; ///< Warning color.
    RgbColor error;   ///< Error/negative color.
    RgbColor info;    ///< Informational color.

    // Border colors
    RgbColor border;        ///< Default border color.
    RgbColor borderFocused; ///< Focused element border.
};

/// @brief Complete theme definition for the TUI.
///
/// Provides consistent styling across all components. Includes color palette,
/// pre-configured styles for common use cases, and border style defaults.
struct Theme
{
    // Color palette
    ColorPalette colors;

    // Common styles
    Style textNormal; ///< Normal text style.
    Style textMuted;  ///< Muted/dimmed text.
    Style textBold;   ///< Bold text style.
    Style textAccent; ///< Accented text style.

    // Interactive elements
    Style buttonNormal;   ///< Normal button state.
    Style buttonFocused;  ///< Focused button state.
    Style buttonDisabled; ///< Disabled button state.

    // List styles
    Style listItem;         ///< Normal list item.
    Style listItemSelected; ///< Selected list item.
    Style listItemDisabled; ///< Disabled list item.

    // Input styles
    Style inputNormal;      ///< Normal input field.
    Style inputFocused;     ///< Focused input field.
    Style inputPlaceholder; ///< Input placeholder text.

    // Dialog styles
    Style dialogBorder;     ///< Dialog border style.
    Style dialogTitle;      ///< Dialog title style.
    Style dialogBackground; ///< Dialog background style.

    // Status bar styles
    Style statusBackground; ///< Status bar background.
    Style statusKey;        ///< Keyboard shortcut key.
    Style statusAction;     ///< Keyboard shortcut action.

    // Semantic styles
    Style success; ///< Success message style.
    Style warning; ///< Warning message style.
    Style error;   ///< Error message style.
    Style info;    ///< Info message style.

    // Completion styles
    Style ghostText;          ///< Ghost text suggestion (dimmed inline suggestion).
    Style completionItem;     ///< Normal completion menu item.
    Style completionSelected; ///< Selected completion menu item.
    Style completionDesc;     ///< Description text in completion menu.
    Style completionMatch;    ///< Highlighted matched characters in fuzzy completion.
    Style completionDetail;   ///< Detail panel text in completion popup.

    // Prompt color palette
    /// @brief Colors used by the shell prompt modules and layout engine.
    struct PromptColorPalette
    {
        RgbColor path;           ///< Path module text color.
        RgbColor gitClean;       ///< Git branch name when clean.
        RgbColor gitDirty;       ///< Git branch name when dirty (unstaged changes).
        RgbColor gitStaged;      ///< Git indicator when staged changes exist.
        RgbColor indicator;      ///< Input line indicator (e.g., "> ").
        RgbColor indicatorError; ///< Indicator color when last command failed.
        RgbColor exitCode;       ///< Exit code badge color.
        RgbColor duration;       ///< Duration badge color.
        RgbColor hostname;       ///< Hostname text color.
        RgbColor username;       ///< Username text color.
        RgbColor background;     ///< Prompt background color.
        RgbColor separator;      ///< Separator/bar color.
        RgbColor badge;          ///< Badge background color.
        RgbColor badgeText;      ///< Badge text color.
        RgbColor clock;          ///< Clock text color.
    } promptColors;

    // Syntax highlighting palette
    /// @brief Colors used by the syntax highlighter for different token categories.
    struct SyntaxHighlightPalette
    {
        RgbColor keyword;     ///< Keywords (if, then, else, match, etc.).
        RgbColor number;      ///< Numeric literals.
        RgbColor string;      ///< String literals.
        RgbColor op;          ///< Operators (+, -, |>, etc.).
        RgbColor variable;    ///< Variables ($VAR).
        RgbColor constructor; ///< Constructors (Some, None, Ok, Error).
        RgbColor punctuation; ///< Punctuation (brackets, semicolons).
        RgbColor comment;     ///< Comments.
        RgbColor type;        ///< Type names.
        RgbColor function;    ///< Shell builtins (cd, export, etc.).
        RgbColor defaultText; ///< Default/unclassified text.
    } syntaxColors;

    // Agent mode color palette
    /// @brief Colors used by the agent mode UI (input bar, spinner, status).
    struct AgentColorPalette
    {
        RgbColor leftBar;           ///< Left bar accent color for agent input/response.
        RgbColor spinnerColor;      ///< Spinner animation color.
        RgbColor errorText;         ///< Error message text color.
        RgbColor statusText;        ///< Status/info text color.
        RgbColor planModeText;      ///< "plan" mode indicator color (green).
        RgbColor executeModeText;   ///< "execute" mode indicator color (red).
        RgbColor pathGradientStart; ///< Path gradient start color (blue).
        RgbColor pathGradientEnd;   ///< Path gradient end color (teal).
    } agentColors;

    // Border style
    BorderStyle borderStyle = BorderStyle::Rounded;
};

/// @brief Returns the default dark theme.
[[nodiscard]] auto darkTheme() -> Theme;

/// @brief Returns a light theme.
[[nodiscard]] auto lightTheme() -> Theme;

/// @brief Returns a minimal monochrome theme.
[[nodiscard]] auto monoTheme() -> Theme;

/// @brief Global theme accessor.
///
/// The TUI uses a global theme for consistent styling. Components can
/// access the current theme through this interface.
class ThemeManager
{
  public:
    /// @brief Returns the singleton instance.
    [[nodiscard]] static auto instance() -> ThemeManager&;

    /// @brief Returns the current theme.
    [[nodiscard]] auto current() const noexcept -> Theme const&;

    /// @brief Sets the current theme.
    void setCurrent(Theme theme);

    /// @brief Resets to the default theme.
    void reset();

  private:
    ThemeManager();
    Theme _current;
};

/// @brief Convenience function to get the current theme.
[[nodiscard]] auto currentTheme() -> Theme const&;

/// @brief Maps a category index to its color from the syntax palette.
///
/// This is the single source of truth for the category-to-color mapping, shared by the generic
/// syntax highlighter (core::tui::HighlightCategory) and by a host application's own highlighter,
/// whose category ordinals line up with HighlightCategory's.
///
/// @param index Category ordinal (matching HighlightCategory's values).
/// @param palette The syntax color palette.
/// @return The color for the category, or defaultText for unknown indices.
[[nodiscard]] constexpr RgbColor categoryColorFromIndex(int index,
                                                        Theme::SyntaxHighlightPalette const& palette) noexcept
{
    switch (index)
    {
        case 0: return palette.defaultText; // Default
        case 1: return palette.keyword;     // Keyword
        case 2: return palette.number;      // Number
        case 3: return palette.string;      // String
        case 4: return palette.op;          // Operator
        case 5: return palette.variable;    // Variable
        case 6: return palette.constructor; // Constructor
        case 7: return palette.comment;     // Comment
        case 8: return palette.type;        // Type
        case 9: return palette.punctuation; // Punctuation
        case 10: return palette.function;   // Function
        case 11: return palette.keyword;    // Preprocessor (tui-only, maps to keyword)
        default: return palette.defaultText;
    }
}

} // namespace core::tui
