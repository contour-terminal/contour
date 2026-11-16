// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <libunicode/codepoint_properties.h>
#include <libunicode/width.h>

#include <algorithm>
#include <string_view>

namespace core::tui
{

/// Calculates the display width of a grapheme cluster.
///
/// This function correctly handles:
/// - Simple characters (width 1)
/// - Wide characters like CJK (width 2)
/// - Emoji with ZWJ sequences (width 2)
/// - Emoji with skin tone modifiers (width 2)
/// - Combining characters (width of base character)
///
/// @param cluster A view of codepoints forming a single grapheme cluster
/// @return The number of terminal columns needed to display the cluster
inline int graphemeClusterWidth(std::u32string_view cluster) noexcept
{
    if (cluster.empty())
        return 0;

    // Get properties of the first codepoint
    auto const props = unicode::codepoint_properties::get(cluster[0]);

    // Emoji with emoji presentation are width 2
    if (props.is_emoji_presentation())
        return 2;

    // Check for ZWJ sequences - if first codepoint is emoji and cluster contains ZWJ
    if (props.is_emoji() && cluster.size() > 1)
    {
        bool const hasZwj = std::ranges::find(cluster, U'\u200D') != cluster.end();
        if (hasZwj)
            return 2;
    }

    // For regular grapheme clusters with combining characters,
    // use the width of the base character (first codepoint)
    int const baseWidth = static_cast<int>(unicode::width(cluster[0]));

    // Ensure at least width 1 for non-zero-width clusters
    return baseWidth > 0 ? baseWidth : 1;
}

/// @brief Calculates the display width of a UTF-8 string in terminal columns.
/// @param text UTF-8 encoded text.
/// @return Total display width in columns.
int stringWidth(std::string_view text) noexcept;

} // namespace core::tui
