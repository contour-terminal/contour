// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/SearchQuery.hpp>

#include <vtbackend/core/RegexEngine.hpp>

#include <libunicode/case_mapping.h>
#include <libunicode/normalization.h>

#include <utility>

namespace vtbackend
{

namespace
{
    /// The characters a regex gives a meaning of their own, which escapeSearchRegex() escapes.
    constexpr auto RegexMetacharacters = std::u32string_view { U"\\^$.|?*+()[]{}" };

    /// Appends @p codepoint to @p units as the regex engine's character type spells it: one unit where
    /// @c wchar_t is UTF-32 (macOS, Linux), a surrogate pair above U+FFFF where it is UTF-16 (Windows).
    void appendUnits(std::wstring& units, char32_t codepoint)
    {
        if constexpr (sizeof(wchar_t) >= sizeof(char32_t))
            units.push_back(static_cast<wchar_t>(codepoint));
        else if (codepoint < 0x10000)
            units.push_back(static_cast<wchar_t>(codepoint));
        else
        {
            auto const offset = codepoint - 0x10000;
            units.push_back(static_cast<wchar_t>(0xD800 + (offset >> 10)));
            units.push_back(static_cast<wchar_t>(0xDC00 + (offset & 0x3FF)));
        }
    }

    /// @p codepoint as a comparison under @p comparison sees it.
    [[nodiscard]] char32_t foldedFor(char32_t codepoint, CaseComparison comparison) noexcept
    {
        switch (comparison)
        {
            case CaseComparison::Folded: return unicode::simple_lowercase(codepoint);
            case CaseComparison::Exact: break;
        }
        return codepoint;
    }

    /// Whether a character in @p role is one of the regex's own, rather than one an escape names.
    [[nodiscard]] constexpr bool isUnescaped(RegexSourceRole role) noexcept
    {
        switch (role)
        {
            case RegexSourceRole::Plain:
            case RegexSourceRole::InClass: return true;
            case RegexSourceRole::Escaped:
            case RegexSourceRole::EscapedInClass: return false;
        }
        return false;
    }

    /// The characters of @p source that smart case reads: those the regex spells itself, not its escapes.
    [[nodiscard]] std::u32string unescapedCharactersOf(std::u32string_view source)
    {
        auto characters = std::u32string {};
        (void) anyRegexSourceCharacter(source, [&](char32_t ch, RegexSourceRole role) {
            if (isUnescaped(role))
                characters.push_back(ch);
            return false;
        });
        return characters;
    }

    /// Whether a class in @p source holds a range whose ends are reversed, as `[z-a]`.
    ///
    /// The standard calls that an error, and libstdc++ and newer libc++ throw for it; Apple's libc++ compiles
    /// it into a class that matches nothing. Refused here, so a pattern is invalid on every platform alike.
    /// An end spelled by an escape is not judged.
    [[nodiscard]] bool hasReversedClassRange(std::u32string_view source)
    {
        auto low = std::optional<char32_t> {};
        auto inRange = false;
        return anyRegexSourceCharacter(source, [&](char32_t ch, RegexSourceRole role) {
            auto const endsRun = role != RegexSourceRole::InClass || ch == U']' || ch == U'\\';
            if (endsRun)
            {
                low.reset();
                inRange = false;
                return false;
            }
            if (inRange)
            {
                auto const reversed = *low > ch;
                low.reset();
                inRange = false;
                return reversed;
            }
            if (ch == U'-' && low)
            {
                inRange = true;
                return false;
            }
            low = ch;
            return false;
        });
    }

    /// A literal needle as the subject spells it: NFC, folded under @p comparison, in engine units.
    [[nodiscard]] std::wstring literalUnits(std::u32string_view pattern, CaseComparison comparison)
    {
        auto units = std::wstring {};
        for (auto const codepoint: unicode::to_nfc(pattern))
            appendUnits(units, foldedFor(codepoint, comparison));
        return units;
    }

    /// A regex's source as the engine compiles it: NFC, folded outside its escapes under @p comparison.
    [[nodiscard]] std::wstring regexUnits(std::u32string_view pattern, CaseComparison comparison)
    {
        auto units = std::wstring {};
        auto const normalized = unicode::to_nfc(pattern);
        (void) anyRegexSourceCharacter(
            std::u32string_view(normalized), [&](char32_t ch, RegexSourceRole role) {
                appendUnits(units, isUnescaped(role) ? foldedFor(ch, comparison) : ch);
                return false;
            });
        return units;
    }
} // namespace

SearchQuery::SearchQuery(std::u32string pattern,
                         SearchOptions options,
                         CaseComparison comparison,
                         std::u32string wordDelimiters,
                         std::wstring needle,
                         std::optional<std::wregex> regex):
    _pattern { std::move(pattern) },
    _options { options },
    _comparison { comparison },
    _wordDelimiters { std::move(wordDelimiters) },
    _needle { std::move(needle) },
    _regex { std::move(regex) }
{
}

std::expected<SearchQuery, SearchPatternError> SearchQuery::compile(std::u32string_view pattern,
                                                                    SearchOptions options,
                                                                    std::u32string_view wordDelimiters)
{
    switch (options.syntax)
    {
        case SearchSyntax::Literal: {
            auto const comparison = caseComparisonFor(pattern, options.caseSensitivity);
            return SearchQuery {
                std::u32string(pattern),           options,     comparison, std::u32string(wordDelimiters),
                literalUnits(pattern, comparison), std::nullopt
            };
        }
        case SearchSyntax::Regex: break;
    }

    if (hasRegexBackReference(pattern))
        return std::unexpected { SearchPatternError::BackReference };

    if (hasReversedClassRange(pattern))
        return std::unexpected { SearchPatternError::InvalidSyntax };

    auto const comparison = caseComparisonFor(unescapedCharactersOf(pattern), options.caseSensitivity);
    try
    {
        auto regex = std::wregex(regexUnits(pattern, comparison), RegexEngineFlags);
        return SearchQuery { std::u32string(pattern),        options,         comparison,
                             std::u32string(wordDelimiters), std::wstring {}, std::move(regex) };
    }
    catch (std::regex_error const&)
    {
        return std::unexpected { SearchPatternError::InvalidSyntax };
    }
}

std::u32string escapeSearchRegex(std::u32string_view text)
{
    auto escaped = std::u32string {};
    escaped.reserve(text.size());
    for (auto const ch: text)
    {
        if (RegexMetacharacters.contains(ch))
            escaped.push_back(U'\\');
        escaped.push_back(ch);
    }
    return escaped;
}

} // namespace vtbackend
