// SPDX-License-Identifier: Apache-2.0
#include <core/tui/completer/SmartCaseMatch.hpp>

#include <algorithm>
#include <cctype>
#include <ranges>

namespace core::tui::completer
{

bool SmartCaseMatch::hasUppercase(std::string_view s) noexcept
{
    return std::ranges::any_of(s, [](unsigned char c) { return std::isupper(c) != 0; });
}

bool SmartCaseMatch::matchesPrefixCaseInsensitive(std::string_view text, std::string_view pattern) noexcept
{
    if (pattern.size() > text.size())
        return false;

    for (auto const i: std::views::iota(std::size_t { 0 }, pattern.size()))
    {
        if (std::tolower(static_cast<unsigned char>(text[i]))
            != std::tolower(static_cast<unsigned char>(pattern[i])))
        {
            return false;
        }
    }
    return true;
}

bool SmartCaseMatch::matchesPrefixCaseSensitive(std::string_view text, std::string_view pattern) noexcept
{
    if (pattern.size() > text.size())
        return false;

    return text.starts_with(pattern);
}

bool SmartCaseMatch::matchesPrefix(std::string_view text, std::string_view pattern) noexcept
{
    if (hasUppercase(pattern))
        return matchesPrefixCaseSensitive(text, pattern);
    else
        return matchesPrefixCaseInsensitive(text, pattern);
}

bool SmartCaseMatch::matchesExact(std::string_view text, std::string_view pattern) noexcept
{
    if (text.size() != pattern.size())
        return false;

    return matchesPrefix(text, pattern);
}

int SmartCaseMatch::adjustScore(int baseScore,
                                std::string_view text,
                                std::string_view pattern,
                                SmartCaseConfig const& config) noexcept
{
    int score = baseScore;

    // Check for exact case-sensitive match (highest bonus)
    if (text == pattern)
    {
        score += config.exactMatchBonus;
    }
    // Check for case-exact prefix match (text starts with pattern, case-sensitive)
    else if (matchesPrefixCaseSensitive(text, pattern))
    {
        score += config.caseExactPrefixBonus;
    }
    // Otherwise no bonus (case-insensitive match only)

    return score;
}

} // namespace core::tui::completer
