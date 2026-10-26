// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <regex>
#include <string_view>

namespace vtbackend
{

// The one regex engine configuration the terminal runs user patterns on, and the source scans that judge a
// pattern before it is compiled. Shared by the link patterns (links/) and the search (screen/), so both
// refuse the same patterns for the same reasons and run them on the same executor.

#ifdef __GLIBCXX__
/// libstdc++'s default (depth-first) executor recursed once per matched character until libstdc++ 16, at
/// 150-350 bytes of stack each: a long run overflowed even an 8 MiB stack. Its breadth-first executor, an
/// extension, keeps the stack to the pattern's size and has no back-references. Chosen on every
/// libstdc++, so that the engine tested here is the one every Linux build runs.
inline constexpr auto RegexExecutorFlags = std::regex_constants::__polynomial;
#else
/// libc++ matches iteratively, and MSVC gives up with `error_stack`: the default engine is fine there.
inline constexpr auto RegexExecutorFlags = std::regex_constants::syntax_option_type {};
#endif

/// The flags every user-supplied pattern is compiled with: ECMAScript, optimized for matching many times,
/// on the executor @ref RegexExecutorFlags picks.
inline constexpr std::regex_constants::syntax_option_type RegexEngineFlags =
    std::regex_constants::ECMAScript | std::regex_constants::optimize | RegexExecutorFlags;

/// What a character of a pattern's source is to the regex grammar.
enum class RegexSourceRole : uint8_t
{
    Plain = 0,      ///< Outside any `[…]` class, not escaped.
    Escaped,        ///< Right after a backslash, outside a class.
    InClass,        ///< Inside `[…]`, not escaped.
    EscapedInClass, ///< Right after a backslash, inside `[…]`.
};

/// Whether some character of @p source, in the role it has there, passes @p test.
/// @param source A regex's source text, in any character type.
/// @param test   Called as `test(Char, RegexSourceRole)`, in source order, until it returns true.
/// @return Whether @p test returned true for any character.
template <typename Char, typename Test>
[[nodiscard]] constexpr bool anyRegexSourceCharacter(std::basic_string_view<Char> source, Test test)
{
    auto escaped = false;
    auto inClass = false;
    for (auto const ch: source)
    {
        auto role = inClass ? RegexSourceRole::InClass : RegexSourceRole::Plain;
        if (escaped)
            role = inClass ? RegexSourceRole::EscapedInClass : RegexSourceRole::Escaped;
        if (test(ch, role))
            return true;
        if (escaped)
            escaped = false;
        else if (ch == Char { '\\' })
            escaped = true;
        else if (!inClass && ch == Char { '[' })
            inClass = true;
        else if (inClass && ch == Char { ']' })
            inClass = false;
    }
    return false;
}

/// Whether @p source holds a back-reference, `\1` to `\9` (and on) outside a class.
///
/// Refused wherever a user pattern is compiled: libstdc++'s breadth-first executor has none, so a pattern
/// using one would match on one platform and not on another.
/// @param source A regex's source text.
/// @return Whether it names a back-reference.
template <typename Char>
[[nodiscard]] constexpr bool hasRegexBackReference(std::basic_string_view<Char> source) noexcept
{
    return anyRegexSourceCharacter(source, [](Char ch, RegexSourceRole role) {
        return role == RegexSourceRole::Escaped && ch >= Char { '1' } && ch <= Char { '9' };
    });
}

} // namespace vtbackend
