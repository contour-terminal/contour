// SPDX-License-Identifier: Apache-2.0
#include <core/platform/GlobMatch.hpp>

#include <cstddef>
#include <string_view>

namespace core::platform
{

namespace
{
    /// Matches one character against the bracket expression that starts at @p pi.
    ///
    /// @param ch The filename character to match.
    /// @param pattern The whole pattern.
    /// @param pi In: the index just past the opening '['. Out: the index just past the closing
    ///           ']', or the pattern's end if there is none.
    /// @return Whether @p ch is in the set, or out of it for a set negated by '!' or '^'.
    [[nodiscard]] bool matchBracket(char ch, std::string_view pattern, std::size_t& pi) noexcept
    {
        auto negate = false;
        auto matched = false;
        if (pi < pattern.size() && (pattern[pi] == '!' || pattern[pi] == '^'))
        {
            negate = true;
            ++pi;
        }
        while (pi < pattern.size() && pattern[pi] != ']')
        {
            if (pi + 2 < pattern.size() && pattern[pi + 1] == '-' && pattern[pi + 2] != ']')
            {
                if (ch >= pattern[pi] && ch <= pattern[pi + 2])
                    matched = true;
                pi += 3;
            }
            else
            {
                if (ch == pattern[pi])
                    matched = true;
                ++pi;
            }
        }
        if (pi < pattern.size())
            ++pi;
        return matched != negate;
    }

    /// @brief Finds the ']' that closes a bracket expression.
    ///
    /// A '[' that no ']' closes is a literal '[', the way fnmatch(3) reads it, so the caller has
    /// to know before it commits to the bracket arm. The scan skips a leading negation character
    /// and takes the first ']' after it, which is where matchBracket() stops too.
    ///
    /// @param pattern The whole pattern.
    /// @param pi The index just past the opening '['.
    /// @return The index of the closing ']', or npos when there is none.
    [[nodiscard]] std::size_t findBracketEnd(std::string_view pattern, std::size_t pi) noexcept
    {
        if (pi < pattern.size() && (pattern[pi] == '!' || pattern[pi] == '^'))
            ++pi;
        return pattern.find(']', pi);
    }
} // namespace

bool globMatchFilename(std::string_view filename, std::string_view pattern)
{
    std::size_t fi = 0;
    std::size_t pi = 0;
    std::size_t starIdx = std::string_view::npos;
    std::size_t matchIdx = 0;

    // Retries the last `*` one character further into the filename, if there was one.
    auto const backtrack = [&]() noexcept {
        if (starIdx == std::string_view::npos)
            return false;
        pi = starIdx + 1;
        ++matchIdx;
        fi = matchIdx;
        return true;
    };

    while (fi < filename.size())
    {
        if (pi < pattern.size() && pattern[pi] == '*')
        {
            starIdx = pi;
            matchIdx = fi;
            ++pi;
        }
        else if (pi < pattern.size() && pattern[pi] == '['
                 && findBracketEnd(pattern, pi + 1) != std::string_view::npos)
        {
            // Before the literal arm below, which would otherwise consume the '[' that opens a
            // bracket expression -- so `[[]`, POSIX's way to match a literal bracket, could never
            // match one.
            ++pi;
            if (matchBracket(filename[fi], pattern, pi))
                ++fi;
            else if (!backtrack())
                return false;
        }
        else if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == filename[fi]))
        {
            ++fi;
            ++pi;
        }
        else if (!backtrack())
        {
            return false;
        }
    }

    while (pi < pattern.size() && pattern[pi] == '*')
        ++pi;

    return pi == pattern.size();
}

} // namespace core::platform
