// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace core::tui::completer
{

/// @brief Configuration for smart case matching score adjustments.
struct SmartCaseConfig
{
    int exactMatchBonus = 50;      ///< Bonus for exact match (case-sensitive).
    int caseExactPrefixBonus = 25; ///< Bonus for case-exact prefix match.
};

/// @brief Smart case-sensitive matching utilities.
///
/// Implements "smart case" matching where:
/// - If the pattern is all lowercase, matching is case-insensitive
/// - If the pattern contains any uppercase letters, matching is case-sensitive
///
/// This matches the behavior of tools like Vim's smartcase and ripgrep's -S flag.
///
/// Usage:
/// @code
///   // Case-insensitive (pattern is all lowercase)
///   SmartCaseMatch::matchesPrefix("FooBar", "foo");  // true
///
///   // Case-sensitive (pattern has uppercase)
///   SmartCaseMatch::matchesPrefix("FooBar", "Foo");  // true
///   SmartCaseMatch::matchesPrefix("foobar", "Foo");  // false
///
///   // Score adjustment
///   int score = SmartCaseMatch::adjustScore(100, "foobar", "foo");
///   // Returns 100 + caseExactPrefixBonus if case matches exactly
/// @endcode
struct SmartCaseMatch
{
    /// @brief Checks if the string contains any uppercase ASCII letters.
    [[nodiscard]] static bool hasUppercase(std::string_view s) noexcept;

    /// @brief Checks if text starts with pattern using smart case rules.
    ///
    /// @param text The text to check.
    /// @param pattern The pattern to match against.
    /// @return true if text starts with pattern (using smart case rules).
    [[nodiscard]] static bool matchesPrefix(std::string_view text, std::string_view pattern) noexcept;

    /// @brief Checks if text exactly equals pattern using smart case rules.
    ///
    /// @param text The text to check.
    /// @param pattern The pattern to match against.
    /// @return true if text equals pattern (using smart case rules).
    [[nodiscard]] static bool matchesExact(std::string_view text, std::string_view pattern) noexcept;

    /// @brief Adjusts a base score based on match quality.
    ///
    /// Applies bonuses for:
    /// - Exact match (text == pattern, case-sensitive): +exactMatchBonus
    /// - Case-exact prefix (text starts with pattern, case-sensitive): +caseExactPrefixBonus
    ///
    /// @param baseScore The initial score.
    /// @param text The completion text.
    /// @param pattern The user's input pattern.
    /// @param config Score bonus configuration.
    /// @return Adjusted score with bonuses applied.
    [[nodiscard]] static int adjustScore(int baseScore,
                                         std::string_view text,
                                         std::string_view pattern,
                                         SmartCaseConfig const& config = {}) noexcept;

    /// @brief Case-insensitive prefix check.
    [[nodiscard]] static bool matchesPrefixCaseInsensitive(std::string_view text,
                                                           std::string_view pattern) noexcept;

    /// @brief Case-sensitive prefix check.
    [[nodiscard]] static bool matchesPrefixCaseSensitive(std::string_view text,
                                                         std::string_view pattern) noexcept;
};

} // namespace core::tui::completer
