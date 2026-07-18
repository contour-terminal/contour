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

/// Replaces a leading home directory with `~`, the way a shell prompt writes a path.
///
/// Purely presentational, for the tab hover tooltip: a caller that wants a path to ACT on wants the real
/// one, which is why this is not done at the source.
///
/// Only a whole path component matches, so a sibling directory whose name merely starts with the home
/// path (`/home/bobby` against a home of `/home/bob`) is left alone rather than mangled into `~by`.
///
/// @param path The absolute path to abbreviate.
/// @param home The user's home directory. An empty value abbreviates nothing.
/// @return @p path with the home prefix replaced by `~`, or @p path unchanged.
[[nodiscard]] std::string abbreviateHomePath(std::string_view path, std::string_view home);

} // namespace contour
