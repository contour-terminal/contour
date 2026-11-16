// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Theme.hpp>

namespace core::tui
{

auto darkTheme() -> Theme
{
    auto theme = Theme {};

    // Color palette - Dark theme
    theme.colors = ColorPalette {
        .primary = 0x82B4FF_rgb,       // Light blue
        .secondary = 0xB482FF_rgb,     // Light purple
        .accent = 0xFFB482_rgb,        // Light orange
        .background = 0x1E1E24_rgb,    // Dark gray
        .surface = 0x282A36_rgb,       // Slightly lighter
        .overlay = 0x323440_rgb,       // Dialog background
        .text = 0xF8F8F2_rgb,          // Off-white
        .textMuted = 0x8C8C96_rgb,     // Gray
        .textInverse = 0x1E1E24_rgb,   // Dark for light bg
        .success = 0x50FA7B_rgb,       // Green
        .warning = 0xFFB86C_rgb,       // Orange
        .error = 0xFF5555_rgb,         // Red
        .info = 0x8BE9FD_rgb,          // Cyan
        .border = 0x44475A_rgb,        // Subtle border
        .borderFocused = 0x82B4FF_rgb, // Primary accent
    };

    // Text styles
    theme.textNormal.fg = theme.colors.text;
    theme.textMuted.fg = theme.colors.textMuted;
    theme.textMuted.dim = true;
    theme.textBold.fg = theme.colors.text;
    theme.textBold.bold = true;
    theme.textAccent.fg = theme.colors.primary;
    theme.textAccent.bold = true;

    // Button styles
    theme.buttonNormal.fg = theme.colors.text;
    theme.buttonFocused.fg = theme.colors.primary;
    theme.buttonFocused.bold = true;
    theme.buttonFocused.inverse = true;
    theme.buttonDisabled.fg = theme.colors.textMuted;
    theme.buttonDisabled.dim = true;

    // List styles
    theme.listItem.fg = theme.colors.text;
    theme.listItemSelected.fg = theme.colors.primary;
    theme.listItemSelected.bold = true;
    theme.listItemDisabled.fg = theme.colors.textMuted;
    theme.listItemDisabled.dim = true;

    // Input styles
    theme.inputNormal.fg = theme.colors.text;
    theme.inputFocused.fg = theme.colors.text;
    theme.inputPlaceholder.fg = theme.colors.textMuted;
    theme.inputPlaceholder.dim = true;

    // Dialog styles
    theme.dialogBorder.fg = theme.colors.border;
    theme.dialogTitle.fg = theme.colors.primary;
    theme.dialogTitle.bold = true;
    theme.dialogBackground.bg = theme.colors.overlay;

    // Status bar styles
    theme.statusBackground.bg = theme.colors.surface;
    theme.statusKey.fg = theme.colors.primary;
    theme.statusKey.bold = true;
    theme.statusAction.fg = theme.colors.textMuted;

    // Semantic styles
    theme.success.fg = theme.colors.success;
    theme.warning.fg = theme.colors.warning;
    theme.error.fg = theme.colors.error;
    theme.info.fg = theme.colors.info;

    // Completion styles
    theme.ghostText.dim = true;                         // Default ghost text is dim (SGR 2)
    theme.completionItem.fg = theme.colors.text;        // Normal menu item
    theme.completionSelected.fg = theme.colors.primary; // Selected item
    theme.completionSelected.inverse = true;            // Inverse for visibility
    theme.completionDesc.fg = theme.colors.textMuted;   // Description text
    theme.completionMatch.fg = theme.colors.info;       // Highlighted matched chars (cyan)
    theme.completionMatch.bold = true;
    theme.completionDetail.fg = theme.colors.text;    // Detail panel text
    theme.completionDetail.bg = theme.colors.overlay; // Detail panel background

    // Prompt colors - Tokyo Night inspired
    theme.promptColors = {
        .path = 0x61AFEF_rgb,           // Teal/soft blue
        .gitClean = 0x50FA7B_rgb,       // Green
        .gitDirty = 0xFFB86C_rgb,       // Orange
        .gitStaged = 0x8BE9FD_rgb,      // Cyan
        .indicator = 0x82B4FF_rgb,      // Blue
        .indicatorError = 0xFF5555_rgb, // Red
        .exitCode = 0xFF5555_rgb,       // Red
        .duration = 0xFFB86C_rgb,       // Orange
        .hostname = 0xB482FF_rgb,       // Purple
        .username = 0x82E0A0_rgb,       // Soft green
        .background = 0x2D3237_rgb,     // Soft gray
        .separator = 0x61AFEF_rgb,      // Soft blue (left bar)
        .badge = 0x3C4148_rgb,          // Dark badge bg
        .badgeText = 0xB4B4B4_rgb,      // Light gray
        .clock = 0x8C8C96_rgb,          // Muted
    };

    // Syntax highlighting - One Dark inspired
    theme.syntaxColors = {
        .keyword = 0xC678DD_rgb,     // Purple
        .number = 0xD19A66_rgb,      // Warm orange
        .string = 0x98C379_rgb,      // Green
        .op = 0x56B6C2_rgb,          // Cyan
        .variable = 0xE06C75_rgb,    // Soft red
        .constructor = 0xE5C07B_rgb, // Yellow
        .punctuation = 0xABB2BF_rgb, // Subtle gray
        .comment = 0x7F848E_rgb,     // Dim gray
        .type = 0xE5C07B_rgb,        // Yellow
        .function = 0x61AFEF_rgb,    // Blue (shell builtins)
        .defaultText = 0xDCDCDC_rgb, // Default text
    };

    // Agent mode colors
    theme.agentColors = {
        .leftBar = 0xAA55FF_rgb,
        .spinnerColor = 0xAA55FF_rgb,
        .errorText = 0xFF5555_rgb,
        .statusText = 0x888888_rgb,
        .planModeText = 0x4ADE80_rgb,    // Spring green
        .executeModeText = 0xEF4444_rgb, // Strong red
        // The path gradient runs blue → teal. A host with its own brand colours replaces the
        // pair; these two are the default theme's, not any one application's.
        .pathGradientStart = 0x5078FF_rgb, // Blue
        .pathGradientEnd = 0x00DCC8_rgb,   // Teal
    };

    theme.borderStyle = BorderStyle::Rounded;

    return theme;
}

auto lightTheme() -> Theme
{
    auto theme = Theme {};

    // Color palette - Light theme
    theme.colors = ColorPalette {
        .primary = 0x0064C8_rgb,       // Blue
        .secondary = 0x6400B4_rgb,     // Purple
        .accent = 0xC86400_rgb,        // Orange
        .background = 0xFAFAFA_rgb,    // Off-white
        .surface = 0xF0F0F5_rgb,       // Light gray
        .overlay = 0xFFFFFF_rgb,       // White
        .text = 0x282A36_rgb,          // Dark gray
        .textMuted = 0x787882_rgb,     // Medium gray
        .textInverse = 0xFFFFFF_rgb,   // White
        .success = 0x009650_rgb,       // Green
        .warning = 0xC88200_rgb,       // Orange
        .error = 0xC83232_rgb,         // Red
        .info = 0x0082B4_rgb,          // Cyan
        .border = 0xC8C8D2_rgb,        // Light border
        .borderFocused = 0x0064C8_rgb, // Primary
    };

    // Apply same style patterns as dark theme
    theme.textNormal.fg = theme.colors.text;
    theme.textMuted.fg = theme.colors.textMuted;
    theme.textBold.fg = theme.colors.text;
    theme.textBold.bold = true;
    theme.textAccent.fg = theme.colors.primary;
    theme.textAccent.bold = true;

    theme.buttonNormal.fg = theme.colors.text;
    theme.buttonFocused.fg = theme.colors.primary;
    theme.buttonFocused.bold = true;
    theme.buttonDisabled.fg = theme.colors.textMuted;
    theme.buttonDisabled.dim = true;

    theme.listItem.fg = theme.colors.text;
    theme.listItemSelected.fg = theme.colors.primary;
    theme.listItemSelected.bold = true;
    theme.listItemDisabled.fg = theme.colors.textMuted;
    theme.listItemDisabled.dim = true;

    theme.inputNormal.fg = theme.colors.text;
    theme.inputFocused.fg = theme.colors.text;
    theme.inputPlaceholder.fg = theme.colors.textMuted;

    theme.dialogBorder.fg = theme.colors.border;
    theme.dialogTitle.fg = theme.colors.primary;
    theme.dialogTitle.bold = true;
    theme.dialogBackground.bg = theme.colors.overlay;

    theme.statusBackground.bg = theme.colors.surface;
    theme.statusKey.fg = theme.colors.primary;
    theme.statusKey.bold = true;
    theme.statusAction.fg = theme.colors.textMuted;

    theme.success.fg = theme.colors.success;
    theme.warning.fg = theme.colors.warning;
    theme.error.fg = theme.colors.error;
    theme.info.fg = theme.colors.info;

    // Completion styles
    theme.ghostText.dim = true;
    theme.completionItem.fg = theme.colors.text;
    theme.completionSelected.fg = theme.colors.primary;
    theme.completionSelected.inverse = true;
    theme.completionDesc.fg = theme.colors.textMuted;
    theme.completionMatch.fg = theme.colors.info;
    theme.completionMatch.bold = true;
    theme.completionDetail.fg = theme.colors.text;
    theme.completionDetail.bg = theme.colors.overlay;

    // Prompt colors - Light theme
    theme.promptColors = {
        .path = 0x0064C8_rgb,           // Blue
        .gitClean = 0x009650_rgb,       // Green
        .gitDirty = 0xC88200_rgb,       // Orange
        .gitStaged = 0x0082B4_rgb,      // Cyan
        .indicator = 0x0064C8_rgb,      // Blue
        .indicatorError = 0xC83232_rgb, // Red
        .exitCode = 0xC83232_rgb,       // Red
        .duration = 0xC88200_rgb,       // Orange
        .hostname = 0x6400B4_rgb,       // Purple
        .username = 0x008250_rgb,       // Green
        .background = 0xEBEDF0_rgb,     // Light gray
        .separator = 0x0064C8_rgb,      // Blue
        .badge = 0xDCE1E6_rgb,          // Light badge bg
        .badgeText = 0x3C3E46_rgb,      // Dark text
        .clock = 0x787882_rgb,          // Muted
    };

    // Syntax highlighting - Light theme
    theme.syntaxColors = {
        .keyword = 0x963CB4_rgb,     // Purple
        .number = 0xB46E32_rgb,      // Brown/orange
        .string = 0x3C8C3C_rgb,      // Green
        .op = 0x1E788C_rgb,          // Teal
        .variable = 0xB4323C_rgb,    // Red
        .constructor = 0xA0821E_rgb, // Dark yellow
        .punctuation = 0x646973_rgb, // Gray
        .comment = 0x8C919B_rgb,     // Light gray
        .type = 0xA0821E_rgb,        // Dark yellow
        .function = 0x0064C8_rgb,    // Blue (shell builtins)
        .defaultText = 0x282A36_rgb, // Dark
    };

    // Agent mode colors
    theme.agentColors = {
        .leftBar = 0x6400B4_rgb,
        .spinnerColor = 0x6400B4_rgb,
        .errorText = 0xC83232_rgb,
        .statusText = 0x787882_rgb,
        .planModeText = 0x22C55E_rgb,      // Medium green (light-theme friendly)
        .executeModeText = 0xDC2626_rgb,   // Strong red (light-theme friendly)
        .pathGradientStart = 0x3060D8_rgb, // Blue (darker for light bg)
        .pathGradientEnd = 0x009688_rgb,   // Teal (darker for light bg)
    };

    theme.borderStyle = BorderStyle::Rounded;

    return theme;
}

auto monoTheme() -> Theme
{
    auto theme = Theme {};

    // Monochrome palette
    theme.colors = ColorPalette {
        .primary = 0xFFFFFF_rgb,
        .secondary = 0xC8C8C8_rgb,
        .accent = 0xFFFFFF_rgb,
        .background = 0x000000_rgb,
        .surface = 0x1E1E1E_rgb,
        .overlay = 0x282828_rgb,
        .text = 0xFFFFFF_rgb,
        .textMuted = 0x808080_rgb,
        .textInverse = 0x000000_rgb,
        .success = 0xFFFFFF_rgb,
        .warning = 0xFFFFFF_rgb,
        .error = 0xFFFFFF_rgb,
        .info = 0xFFFFFF_rgb,
        .border = 0x646464_rgb,
        .borderFocused = 0xFFFFFF_rgb,
    };

    // Simple monochrome styles
    theme.textNormal.fg = theme.colors.text;
    theme.textMuted.dim = true;
    theme.textBold.bold = true;
    theme.textAccent.bold = true;
    theme.textAccent.underline = true;

    theme.buttonNormal = {};
    theme.buttonFocused.inverse = true;
    theme.buttonDisabled.dim = true;

    theme.listItem = {};
    theme.listItemSelected.inverse = true;
    theme.listItemDisabled.dim = true;

    theme.inputNormal = {};
    theme.inputFocused = {};
    theme.inputPlaceholder.dim = true;

    theme.dialogBorder = {};
    theme.dialogTitle.bold = true;
    theme.dialogBackground = {};

    theme.statusBackground.inverse = true;
    theme.statusKey.bold = true;
    theme.statusAction = {};

    theme.success.bold = true;
    theme.warning.bold = true;
    theme.warning.underline = true;
    theme.error.bold = true;
    theme.error.inverse = true;
    theme.info.bold = true;

    // Completion styles
    theme.ghostText.dim = true;
    theme.completionItem = {};
    theme.completionSelected.inverse = true;
    theme.completionDesc.dim = true;
    theme.completionMatch.bold = true;
    theme.completionMatch.underline = true;
    theme.completionDetail.fg = theme.colors.text;
    theme.completionDetail.bg = theme.colors.overlay;

    // Prompt colors - Monochrome
    theme.promptColors = {
        .path = 0xFFFFFF_rgb,
        .gitClean = 0xFFFFFF_rgb,
        .gitDirty = 0xC8C8C8_rgb,
        .gitStaged = 0xFFFFFF_rgb,
        .indicator = 0xFFFFFF_rgb,
        .indicatorError = 0xC8C8C8_rgb,
        .exitCode = 0xFFFFFF_rgb,
        .duration = 0xC8C8C8_rgb,
        .hostname = 0xFFFFFF_rgb,
        .username = 0xFFFFFF_rgb,
        .background = 0x1E1E1E_rgb,
        .separator = 0xC8C8C8_rgb,
        .badge = 0x323232_rgb,
        .badgeText = 0xC8C8C8_rgb,
        .clock = 0x808080_rgb,
    };

    // Syntax highlighting - Monochrome
    theme.syntaxColors = {
        .keyword = 0xFFFFFF_rgb,
        .number = 0xC8C8C8_rgb,
        .string = 0xC8C8C8_rgb,
        .op = 0xFFFFFF_rgb,
        .variable = 0xFFFFFF_rgb,
        .constructor = 0xFFFFFF_rgb,
        .punctuation = 0x808080_rgb,
        .comment = 0x808080_rgb,
        .type = 0xFFFFFF_rgb,
        .function = 0xFFFFFF_rgb,
        .defaultText = 0xFFFFFF_rgb,
    };

    // Agent mode colors
    theme.agentColors = {
        .leftBar = 0xC8C8C8_rgb,
        .spinnerColor = 0xC8C8C8_rgb,
        .errorText = 0xFFFFFF_rgb,
        .statusText = 0x808080_rgb,
        .planModeText = 0xFFFFFF_rgb,      // White (monochrome)
        .executeModeText = 0xFFFFFF_rgb,   // White (monochrome)
        .pathGradientStart = 0xC8C8C8_rgb, // Light gray
        .pathGradientEnd = 0xFFFFFF_rgb,   // White
    };

    theme.borderStyle = BorderStyle::Single;

    return theme;
}

// =============================================================================
// ThemeManager
// =============================================================================

ThemeManager::ThemeManager(): _current(darkTheme())
{
}

auto ThemeManager::instance() -> ThemeManager&
{
    static auto manager = ThemeManager {};
    return manager;
}

auto ThemeManager::current() const noexcept -> Theme const&
{
    return _current;
}

void ThemeManager::setCurrent(Theme theme)
{
    _current = theme;
}

void ThemeManager::reset()
{
    _current = darkTheme();
}

auto currentTheme() -> Theme const&
{
    return ThemeManager::instance().current();
}

} // namespace core::tui
