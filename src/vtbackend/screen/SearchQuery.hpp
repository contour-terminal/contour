// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Search.hpp>

#include <expected>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

namespace vtbackend
{

/// A search pattern compiled once, with the options it was compiled under.
///
/// Immutable once compiled: everything a match needs -- the resolved case comparison, the folded needle or
/// the compiled regex, the word delimiters -- is fixed by compile(), so stepping, counting and highlighting
/// all read one answer to "what does this pattern match" and none of them re-derives it.
class SearchQuery
{
  public:
    /// Compiles @p pattern under @p options.
    ///
    /// A literal pattern always compiles. A regex is NFC-normalized, folded outside its escapes where the
    /// comparison folds (so `\D` stays `\D`), and compiled with @ref RegexEngineFlags. Smart case reads a
    /// regex's own characters only -- the unescaped ones and those of a class -- so `\d+` folds and `\dA`
    /// does not.
    ///
    /// @pre !pattern.empty()
    /// @param pattern        The pattern as the user gave it.
    /// @param options        How to read it.
    /// @param wordDelimiters The characters that end a word for SearchWordBoundary::WholeWord.
    /// @return The query, or why the pattern cannot be searched for.
    [[nodiscard]] static std::expected<SearchQuery, SearchPatternError> compile(
        std::u32string_view pattern, SearchOptions options, std::u32string_view wordDelimiters);

    /// @return The pattern as given to compile().
    [[nodiscard]] std::u32string const& pattern() const noexcept { return _pattern; }

    /// @return The options it was compiled under.
    [[nodiscard]] SearchOptions options() const noexcept { return _options; }

    /// @return How letter case is compared: the options' case policy resolved against the pattern.
    [[nodiscard]] CaseComparison comparison() const noexcept { return _comparison; }

  private:
    SearchQuery(std::u32string pattern,
                SearchOptions options,
                CaseComparison comparison,
                std::u32string wordDelimiters,
                std::wstring needle,
                std::optional<std::wregex> regex);

    std::u32string _pattern;           ///< As given.
    SearchOptions _options;            ///< As given.
    CaseComparison _comparison;        ///< The case policy resolved against the pattern.
    std::u32string _wordDelimiters;    ///< What ends a word, read at compile time.
    std::wstring _needle;              ///< Literal: the NFC, folded pattern, one unit per codepoint.
    std::optional<std::wregex> _regex; ///< Regex: the compiled pattern.
};

/// @p text with every regex metacharacter escaped, so a regex of it matches the text literally.
/// @param text Any text.
/// @return @p text with a backslash before each of `\^$.|?*+()[]{}`.
[[nodiscard]] std::u32string escapeSearchRegex(std::u32string_view text);

} // namespace vtbackend
