// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>
#include <vector>

namespace core::tui::completer
{

/// @brief Configuration for fuzzy matching scoring.
struct FuzzyConfig
{
    int maxMatchPercentBonus = 100; ///< Max bonus scaled by match coverage percentage.
    int consecutiveBonus = 10;      ///< Bonus per consecutive match run.
    int wordStartBonus = 15;        ///< Bonus for match at word boundary.
    int prefixMatchBonus = 50;      ///< Extra bonus when match is also a prefix.
    double minMatchThreshold = 0.2; ///< Minimum match quality (0.0-1.0) to accept.
};

/// @brief Result of a fuzzy match operation.
struct FuzzyMatchResult
{
    bool matches = false;            ///< Did the pattern match?
    size_t matchedChars = 0;         ///< Number of pattern graphemes matched.
    size_t consecutiveRuns = 0;      ///< Number of consecutive match sequences.
    size_t longestRun = 0;           ///< Length of longest consecutive run.
    size_t wordStartMatches = 0;     ///< Matches occurring at word boundaries.
    size_t textGraphemeCount = 0;    ///< Total grapheme count in the matched text.
    size_t patternGraphemeCount = 0; ///< Total grapheme count in the pattern.

    /// @brief Grapheme cluster indices of matched characters (for highlighting).
    /// These are visual character positions, not byte offsets.
    std::vector<size_t> positions;

    /// @brief Match quality as ratio (0.0 - 1.0).
    /// @param graphemeCount Total grapheme count in the text.
    [[nodiscard]] double quality(size_t graphemeCount) const noexcept;

    /// @brief True when all matched characters were found consecutively (substring match).
    /// A contiguous substring match is always considered valid regardless of text length.
    [[nodiscard]] bool isContiguousSubstring() const noexcept;
};

/// @brief Fuzzy string matching with grapheme cluster awareness.
///
/// When the pattern occurs verbatim as a contiguous substring, that block is
/// reported (best highlight, ranks ahead of a scattered match). Otherwise the
/// pattern characters are matched in order with gaps allowed (subsequence /
/// abbreviation matching). All operations work on Unicode grapheme clusters,
/// not bytes.
///
/// Examples:
/// - "ds" matches "Downloads" as the substring "...loaDS" -> positions = {7, 8}
/// - "dcm" matches "Documents" (D.c.m...., subsequence) -> positions = {0, 2, 4}
/// - "📁d" matches "📁Downloads" -> positions = {0, 1}
///
/// Integrates with smart case rules:
/// - Lowercase pattern -> case-insensitive fuzzy
/// - Pattern with uppercase -> case-sensitive fuzzy
///
/// Usage:
/// @code
///   auto result = FuzzyMatch::matchSmartCase("Downloads", "ds");
///   if (result.matches) {
///       int score = FuzzyMatch::calculateScore(50, "Downloads", "ds", result);
///       // result.positions = {7, 8} for highlighting the "ds" in "Downloads"
///   }
/// @endcode
struct FuzzyMatch
{
    /// @brief Fuzzy match with explicit case sensitivity.
    /// @param text The text to search in.
    /// @param pattern The pattern to find.
    /// @param caseSensitive Whether to match case-sensitively.
    /// @return Match result with grapheme positions.
    [[nodiscard]] static FuzzyMatchResult match(std::string_view text,
                                                std::string_view pattern,
                                                bool caseSensitive) noexcept;

    /// @brief Fuzzy match using smart case rules.
    /// Lowercase pattern -> case-insensitive; uppercase in pattern -> case-sensitive.
    [[nodiscard]] static FuzzyMatchResult matchSmartCase(std::string_view text,
                                                         std::string_view pattern) noexcept;

    /// @brief Calculate score from fuzzy match result.
    /// @param baseScore Starting score for the completion item.
    /// @param text The matched text.
    /// @param pattern The search pattern.
    /// @param result The match result from match() or matchSmartCase().
    /// @param config Scoring configuration.
    /// @return Adjusted score with fuzzy bonuses applied.
    [[nodiscard]] static int calculateScore(int baseScore,
                                            std::string_view text,
                                            std::string_view pattern,
                                            FuzzyMatchResult const& result,
                                            FuzzyConfig const& config = {}) noexcept;

    /// @brief Check if character is a word boundary.
    /// Currently checks: '/', '_', '-', ' ', '\t', '.'
    /// @note Consider extending to Unicode word boundaries per UAX#29
    /// (Unicode Text Segmentation) in the future. This would require
    /// integrating with libunicode's word boundary detection.
    [[nodiscard]] static bool isWordBoundary(char c) noexcept;

    /// @brief Check if grapheme at given index is at start of a word.
    /// @param text The text to check.
    /// @param graphemeIndex The grapheme cluster index (0-based).
    /// @param graphemeByteOffsets Byte offsets of each grapheme start.
    [[nodiscard]] static bool isWordStart(std::string_view text,
                                          size_t graphemeIndex,
                                          std::vector<size_t> const& graphemeByteOffsets) noexcept;

    /// @brief Count the number of grapheme clusters in text.
    [[nodiscard]] static size_t countGraphemes(std::string_view text) noexcept;
};

} // namespace core::tui::completer
