// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstddef>
#include <string>
#include <vector>

namespace core::tui::completer
{

/// @brief A single completion suggestion.
///
/// CompletionItem represents a completion candidate that can be displayed
/// in a completion popup. It contains:
/// - `text`: The actual text to insert when the completion is accepted
/// - `displayText`: Optional alternative text for display (useful for abbreviations)
/// - `description`: Help text or synopsis shown alongside the completion
/// - `score`: Ranking score for sorting (higher = better match)
/// - `matchPositions`: Grapheme indices of matched characters for highlighting
struct CompletionItem
{
    std::string text {};        ///< Full completion text (the actual value to insert).
    std::string displayText {}; ///< Text to display in menu (may be abbreviated).
    std::string description {}; ///< Help text / synopsis for the completion menu.
    std::string detail {};      ///< Detailed documentation (markdown) for the detail panel.
    int score = 0;              ///< Ranking score (higher = better match).

    /// @brief Grapheme cluster indices of matched characters for highlighting.
    /// Empty for prefix matches (no special highlighting needed).
    /// For fuzzy matches, contains the indices from FuzzyMatchResult::positions.
    std::vector<size_t> matchPositions {};

    /// @brief Returns the suffix to append after the prefix.
    /// @param prefixLen Length of the prefix already typed.
    [[nodiscard]] std::string suffix(size_t prefixLen) const
    {
        return prefixLen < text.size() ? text.substr(prefixLen) : "";
    }

    auto operator<=>(CompletionItem const&) const = default;
};

} // namespace core::tui::completer
