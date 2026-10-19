// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// Why a `url:` template cannot be used.
enum class LinkUrlTemplateError : uint8_t
{
    Unterminated = 0, ///< A `{` without its `}`.
    NotAGroup,        ///< A `{...}` naming something other than one digit, 0-9.
};

/// Who chose the scheme of what a link opens (§7.6).
enum class LinkSchemeSource : uint8_t
{
    Printed = 0, ///< The printed text: no template, or one that does not start with a `scheme:` of its own.
    Template,    ///< The template's own literal `scheme:`, which the user wrote.
};

/// A user pattern's `url:`: `{0}` is the whole match, `{1}`-`{9}` its capture groups (§4.2).
class LinkUrlTemplate
{
  public:
    /// Finds the groups @p text names.
    /// @param text The template as configured.
    /// @return The template, or why it cannot be used.
    [[nodiscard]] static std::expected<LinkUrlTemplate, LinkUrlTemplateError> parse(std::string_view text);

    /// The highest group the template names, which the pattern must have.
    /// @return The group; -1 when the template names none.
    [[nodiscard]] int highestGroup() const noexcept { return _highestGroup; }

    /// Who chooses the scheme of what the template expands to, decided when it is parsed: the template, where
    /// it starts with a literal `scheme:` (`https://jira.example/{0}`), else the printed text (`{0}`).
    /// @return The chooser.
    [[nodiscard]] LinkSchemeSource schemeSource() const noexcept { return _schemeSource; }

    /// The URL for one match: the template with each group's text inserted, percent-encoded where it falls
    /// outside the URL code points (percentEncodeForUrl()).
    /// @param group The text of group N (0 for the whole match); empty for a group that took no part.
    /// @return The URL.
    [[nodiscard]] std::string expand(std::function<std::string_view(int)> const& group) const;

  private:
    /// One run of the template: literal text, or a group.
    struct Piece
    {
        std::string literal; ///< The text as written, where @ref group is none.
        int group = -1;      ///< The group inserted here; -1 for a literal.
    };

    /// What parse() built.
    /// @param pieces The template's runs, in order.
    /// @param highestGroup The highest group among them; -1 for none.
    /// @param schemeSource Who chooses the scheme of an expansion.
    LinkUrlTemplate(std::vector<Piece> pieces, int highestGroup, LinkSchemeSource schemeSource) noexcept:
        _pieces { std::move(pieces) }, _highestGroup { highestGroup }, _schemeSource { schemeSource }
    {
    }

    std::vector<Piece> _pieces;
    int _highestGroup = -1;
    LinkSchemeSource _schemeSource = LinkSchemeSource::Printed;
};

/// The scheme @p url starts with, as RFC 3986 spells one: a letter, then letters, digits, `+`, `-` or `.`,
/// up to a `:`.
/// @param url A URL, or any text.
/// @return The scheme, without its `:`; nullopt when @p url starts with none.
[[nodiscard]] std::optional<std::string_view> urlSchemeOf(std::string_view url) noexcept;

/// Whether @p url starts with the scheme @p scheme, in any case (RFC 3986). ASCII is folded alone, whatever
/// the locale makes of `I` and `i`.
/// @param url A URL, or any text.
/// @param scheme A scheme in lower case, such as `file`.
/// @return Whether @p url's scheme is @p scheme.
[[nodiscard]] bool hasUrlScheme(std::string_view url, std::string_view scheme) noexcept;

/// Percent-encodes what lies outside the WHATWG URL code points: space, controls, and `"#<>[\]^`{|}`. Keeps
/// ASCII letters and digits, `!$&'()*+,-./:;=?@_~`, `%`, and every byte of non-ASCII text.
/// @param text What a template inserts.
/// @return The text, safe inside a URL.
[[nodiscard]] std::string percentEncodeForUrl(std::string_view text);

} // namespace vtbackend
