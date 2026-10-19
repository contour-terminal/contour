// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/LinkUrlTemplate.hpp>

#include <crispy/ASCII.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace vtbackend
{

namespace
{
    /// The ASCII punctuation a URL holds as it is (WHATWG URL code points), and `%`, which may already
    /// encode.
    constexpr auto KeptPunctuation = std::string_view { "!$&'()*+,-./:;=?@_~%" };

    /// Whether @p byte goes into a URL as it is.
    [[nodiscard]] constexpr bool keeps(unsigned char byte) noexcept
    {
        return byte >= 0x80 || (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z')
               || (byte >= 'a' && byte <= 'z') || KeptPunctuation.contains(static_cast<char>(byte));
    }

    [[nodiscard]] constexpr bool isAsciiLetter(char ch) noexcept
    {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    }

    /// Whether @p ch may follow a scheme's first letter (RFC 3986).
    [[nodiscard]] constexpr bool continuesScheme(char ch) noexcept
    {
        return isAsciiLetter(ch) || (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.';
    }
} // namespace

std::optional<std::string_view> urlSchemeOf(std::string_view url) noexcept
{
    auto const colon = url.find(':');
    if (colon == std::string_view::npos || colon == 0 || !isAsciiLetter(url.front()))
        return std::nullopt;
    auto const scheme = url.substr(0, colon);
    if (!std::ranges::all_of(scheme, continuesScheme))
        return std::nullopt;
    return scheme;
}

bool hasUrlScheme(std::string_view url, std::string_view scheme) noexcept
{
    auto const found = urlSchemeOf(url);
    return found
           && std::ranges::equal(*found, scheme, [](char a, char b) { return crispy::ascii::fold(a) == b; });
}

std::string percentEncodeForUrl(std::string_view text)
{
    auto encoded = std::string {};
    encoded.reserve(text.size());
    for (auto const ch: text)
    {
        if (auto const byte = static_cast<unsigned char>(ch); keeps(byte))
            encoded += ch;
        else
            encoded += std::format("%{:02X}", byte);
    }
    return encoded;
}

std::expected<LinkUrlTemplate, LinkUrlTemplateError> LinkUrlTemplate::parse(std::string_view text)
{
    auto pieces = std::vector<Piece> {};
    auto highestGroup = -1;
    // A walk over the template: literal text up to the next `{`, then the group it names.
    while (!text.empty())
    {
        auto const open = text.find('{');
        if (open != 0)
        {
            pieces.push_back(Piece { .literal = std::string { text.substr(0, open) }, .group = -1 });
            if (open == std::string_view::npos)
                break;
            text.remove_prefix(open);
        }
        auto const close = text.find('}');
        if (close == std::string_view::npos)
            return std::unexpected(LinkUrlTemplateError::Unterminated);
        auto const name = text.substr(1, close - 1);
        if (name.size() != 1 || name.front() < '0' || name.front() > '9')
            return std::unexpected(LinkUrlTemplateError::NotAGroup);
        auto const group = name.front() - '0';
        pieces.push_back(Piece { .literal = {}, .group = group });
        highestGroup = std::max(highestGroup, group);
        text.remove_prefix(close + 1);
    }
    // The user wrote the scheme only where the template starts with it, before any group.
    auto const schemeSource =
        !pieces.empty() && pieces.front().group < 0 && urlSchemeOf(pieces.front().literal).has_value()
            ? LinkSchemeSource::Template
            : LinkSchemeSource::Printed;
    return LinkUrlTemplate { std::move(pieces), highestGroup, schemeSource };
}

std::string LinkUrlTemplate::expand(std::function<std::string_view(int)> const& group) const
{
    auto url = std::string {};
    for (auto const& piece: _pieces)
        url += piece.group < 0 ? piece.literal : percentEncodeForUrl(group(piece.group));
    return url;
}

} // namespace vtbackend
