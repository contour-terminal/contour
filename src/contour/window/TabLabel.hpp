// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>

namespace contour::window
{

/// Values available to a tab-label template when it is expanded.
///
/// Kept in its own dependency-free header (no Qt, no app types) so the template-expansion logic in
/// expandTabLabel() is unit-testable without the full Qt/app manager. The set is intentionally small
/// for v1; new members map one-to-one onto new placeholders in expandTabLabel().
struct TabLabelContext
{
    int position = 1;        ///< The tab's 1-based position, exposed as the {TabPosition} placeholder.
    std::string windowTitle; ///< The active pane's resolved title, exposed as {WindowTitle}.
};

/// Expands a tab-label template into the string shown on a GUI tab.
///
/// The template is a sequence of literal text and `{Name}` placeholders, parsed with
/// crispy::parseInterpolatedString (the same syntax the status line uses). Literal text passes
/// through unchanged. Recognized placeholders (case-sensitive) are substituted from @p ctx:
///   - `{WindowTitle}` → @p ctx.windowTitle
///   - `{TabPosition}` → @p ctx.position (1-based)
/// An unrecognized placeholder is echoed verbatim — its exact original `{...}` slice — matching
/// parseStatusLineSegment so both surfaces show the user what they typed rather than dropping it.
/// Placeholder flags/attributes (e.g. `{WindowTitle:Bold}`) are accepted but ignored, since tab labels
/// are plain text. There is no brace escaping: braces that do not form a recognized placeholder are
/// literal, so a rename like `build {{123}}` shows `build {{123}}` verbatim, while `{WindowTitle}`
/// still tracks the title.
///
/// @param tmpl The template string. It must outlive the call: the parser yields fragments that borrow
///             from @p tmpl, but they are consumed before this function returns, so the result owns no
///             references into @p tmpl.
/// @param ctx  The values to substitute for the supported placeholders.
/// @return The expanded label.
[[nodiscard]] std::string expandTabLabel(std::string_view tmpl, TabLabelContext const& ctx);

/// The label a tab shows: an expanded label left empty names the program in front instead, since a
/// blank tab names nothing. Applies to every edition (an empty label is a defect, not a look).
/// @param expanded    The expanded label (expandTabLabel()).
/// @param processName The session's foreground process name; empty when unknown, as for a remote session.
/// @return @p expanded unless it is empty, else @p processName.
[[nodiscard]] std::string tabLabelWithFallback(std::string expanded, std::string_view processName);

} // namespace contour::window
