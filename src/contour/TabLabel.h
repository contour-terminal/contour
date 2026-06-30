// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>

namespace contour
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
/// crispy::parse_interpolated_string (the same syntax the status line uses). Literal text passes
/// through unchanged. Recognized placeholders (case-sensitive) are substituted from @p ctx:
///   - `{WindowTitle}` → @p ctx.windowTitle
///   - `{TabPosition}` → @p ctx.position (1-based)
/// Any unrecognized placeholder expands to the empty string (matching StatusLineBuilder). Placeholder
/// flags/attributes (e.g. `{WindowTitle:Bold}`) are accepted but ignored, since tab labels are plain
/// text. A malformed, unterminated placeholder (an opening `{` with no closing `}`) is likewise
/// treated as unrecognized and expands to empty. A literal brace is written by doubling it: `{{` and
/// `}}` expand to `{` and `}` (so a rename like `build {{123}}` shows `build {123}` while
/// `{WindowTitle}` still tracks the title).
///
/// @param tmpl The template string. It must outlive the call: the parser yields fragments that borrow
///             from @p tmpl, but they are consumed before this function returns, so the result owns no
///             references into @p tmpl.
/// @param ctx  The values to substitute for the supported placeholders.
/// @return The expanded label.
[[nodiscard]] std::string expandTabLabel(std::string_view tmpl, TabLabelContext const& ctx);

} // namespace contour
