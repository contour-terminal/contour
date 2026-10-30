// SPDX-License-Identifier: Apache-2.0
#include <contour/config/HostUrl.hpp>

#include <crispy/ASCII.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <ranges>
#include <system_error>
#include <utility>

namespace contour::config
{

namespace
{
    /// What each error says, indexed by the enumerator.
    constexpr auto HostUrlErrorText = std::array<std::string_view, 11> {
        "expected a URL such as ssh://user@host or tls://host:port",                     // MissingScheme
        "unknown scheme (expected ssh:// or tls://)",                                    // UnknownScheme
        "the URL names no host",                                                         // MissingHost
        "a host may not begin with '-'",                                                 // OptionLikeHost
        "a user may not begin with '-'",                                                 // OptionLikeUser
        "a host may not contain blanks, control characters or any of '`\"$\\;&<>|(){},", // UnsafeHost
        "a user may not contain control characters or any of '`\"$;&<>|(){}",            // UnsafeUser
        "the port must be a number from 1 to 65535",                                     // BadPort
        "tls:// needs a port (tls://host:port)",                                         // PortRequired
        "a host URL has no path",                                                        // UnexpectedPath
        "tls:// takes no user; the token authenticates",                                 // UserNotAllowed
    };
    static_assert(HostUrlErrorText.size() == std::to_underlying(HostUrlError::UserNotAllowed) + 1);

    /// What the loader logs, and Settings shows, for each HostEntryError, in enumerator order.
    constexpr auto HostEntryErrorText = std::array<std::string_view, 1> {
        "a tls:// host needs a token_file", // TokenRequired
    };
    static_assert(HostEntryErrorText.size() == std::to_underlying(HostEntryError::TokenRequired) + 1);

    /// What a host may not contain beyond control characters, as OpenSSH >= 9.6 refuses on its own command
    /// line: an older ssh hands the name to a user's `ProxyCommand` through `%h` unchecked.
    constexpr auto UnsafeHostCharacters = std::string_view { " '`\"$\\;&<>|(){}," };

    /// What a user may not contain beyond control characters; OpenSSH allows blanks and '\\' in a login.
    constexpr auto UnsafeUserCharacters = std::string_view { "'`\"$;&<>|(){}" };

    /// @return Whether @p text holds a control character or one of @p unsafe.
    [[nodiscard]] bool containsUnsafe(std::string_view text, std::string_view unsafe) noexcept
    {
        return std::ranges::any_of(text, [unsafe](char c) {
            auto const code = static_cast<unsigned char>(c);
            return code < 0x20 || code == 0x7F || unsafe.contains(c);
        });
    }

    /// @return The row of @p name in @ref HostSchemes, compared case-insensitively.
    [[nodiscard]] HostSchemeInfo const* findScheme(std::string_view name) noexcept
    {
        auto const sameSpelling = [name](HostSchemeInfo const& row) {
            return std::ranges::equal(name, row.name, [](char a, char b) {
                return crispy::ascii::fold(a) == crispy::ascii::fold(b);
            });
        };
        auto const found = std::ranges::find_if(HostSchemes, sameSpelling);
        return found == HostSchemes.end() ? nullptr : &*found;
    }

    /// A URL split at its `://`: the scheme's row, and the authority after the separator.
    struct SchemeSplit
    {
        HostSchemeInfo const* scheme;
        std::string_view authority;
    };

    /// Splits @p url at its `://`, what a half-typed address and a whole one both begin with.
    /// @return The scheme and the rest; MissingScheme or UnknownScheme when it names none of @ref
    /// HostSchemes.
    [[nodiscard]] std::expected<SchemeSplit, HostUrlError> splitScheme(std::string_view url) noexcept
    {
        auto const separator = url.find("://");
        if (separator == std::string_view::npos)
            return std::unexpected { HostUrlError::MissingScheme };
        auto const* const scheme = findScheme(url.substr(0, separator));
        if (!scheme)
            return std::unexpected { HostUrlError::UnknownScheme };
        return SchemeSplit { .scheme = scheme, .authority = url.substr(separator + 3) };
    }

    /// @return The row of @p scheme in @ref HostSchemes.
    [[nodiscard]] HostSchemeInfo const& schemeInfo(HostScheme scheme) noexcept
    {
        return *std::ranges::find(HostSchemes, scheme, &HostSchemeInfo::scheme);
    }

    /// Parses a port as `vthost::parseHostPort` does: every character a digit, 1 to 65535.
    [[nodiscard]] std::expected<std::uint16_t, HostUrlError> parsePort(std::string_view text)
    {
        auto port = 0U;
        auto const* const first = text.data();
        auto const* const last = first + text.size();
        auto const [ptr, ec] = std::from_chars(first, last, port);
        if (text.empty() || ec != std::errc {} || ptr != last || port == 0 || port > 65535)
            return std::unexpected { HostUrlError::BadPort };
        return static_cast<std::uint16_t>(port);
    }

    /// The host and the port text of an authority without its user: `host`, `host:port`, `[v6]:port`.
    struct HostPort
    {
        std::string_view host;
        std::optional<std::string_view> port;
    };

    [[nodiscard]] std::expected<HostPort, HostUrlError> splitHostPort(std::string_view text)
    {
        if (text.starts_with('['))
        {
            auto const close = text.find(']');
            if (close == std::string_view::npos)
                return std::unexpected { HostUrlError::MissingHost };
            auto const rest = text.substr(close + 1);
            if (rest.empty())
                return HostPort { .host = text.substr(1, close - 1), .port = std::nullopt };
            if (!rest.starts_with(':'))
                return std::unexpected { HostUrlError::BadPort };
            return HostPort { .host = text.substr(1, close - 1), .port = rest.substr(1) };
        }
        auto const colon = text.find(':');
        if (colon == std::string_view::npos)
            return HostPort { .host = text, .port = std::nullopt };
        return HostPort { .host = text.substr(0, colon), .port = text.substr(colon + 1) };
    }
} // namespace

std::expected<HostUrl, HostUrlError> parseHostUrl(std::string_view url)
{
    auto const split = splitScheme(url);
    if (!split)
        return std::unexpected { split.error() };
    auto const* const scheme = split->scheme;

    auto authority = split->authority;
    if (authority.contains('/'))
        return std::unexpected { HostUrlError::UnexpectedPath };

    auto result = HostUrl { .scheme = scheme->scheme, .user = {}, .host = {}, .port = std::nullopt };
    if (auto const at = authority.rfind('@'); at != std::string_view::npos)
    {
        result.user = std::string { authority.substr(0, at) };
        authority = authority.substr(at + 1);
    }

    auto const hostPort = splitHostPort(authority);
    if (!hostPort)
        return std::unexpected { hostPort.error() };
    if (hostPort->host.empty())
        return std::unexpected { HostUrlError::MissingHost };
    if (hostPort->host.starts_with('-'))
        return std::unexpected { HostUrlError::OptionLikeHost };
    if (result.user.starts_with('-'))
        return std::unexpected { HostUrlError::OptionLikeUser };
    if (containsUnsafe(hostPort->host, UnsafeHostCharacters))
        return std::unexpected { HostUrlError::UnsafeHost };
    if (containsUnsafe(result.user, UnsafeUserCharacters))
        return std::unexpected { HostUrlError::UnsafeUser };
    result.host = std::string { hostPort->host };

    if (hostPort->port)
    {
        auto const port = parsePort(*hostPort->port);
        if (!port)
            return std::unexpected { port.error() };
        result.port = *port;
    }
    else if (scheme->port == PortRule::Required)
        return std::unexpected { HostUrlError::PortRequired };

    // The TLS transport has no login, so a user there is a typo for something else.
    if (!result.user.empty() && result.scheme != HostScheme::Ssh)
        return std::unexpected { HostUrlError::UserNotAllowed };
    return result;
}

std::string_view describe(HostUrlError error) noexcept
{
    return HostUrlErrorText.at(std::to_underlying(error));
}

std::string_view describe(HostEntryError error) noexcept
{
    return HostEntryErrorText.at(std::to_underlying(error));
}

std::expected<void, HostEntryError> checkHostToken(HostScheme scheme, std::string_view tokenFile) noexcept
{
    // A TCP daemon always requires a token, so a saved TLS host without one could never attach.
    if (scheme == HostScheme::Tls && tokenFile.empty())
        return std::unexpected { HostEntryError::TokenRequired };
    return {};
}

std::optional<HostScheme> hostSchemeOf(std::string_view url) noexcept
{
    auto const split = splitScheme(url);
    return split ? std::optional { split->scheme->scheme } : std::nullopt;
}

std::string_view hostSchemeName(HostScheme scheme) noexcept
{
    return schemeInfo(scheme).name;
}

std::vector<HostFieldProblem> checkHostDraft(HostDraft const& draft)
{
    auto problems = std::vector<HostFieldProblem> {};
    if (auto const url = parseHostUrl(draft.url); !url)
        problems.push_back({ .field = HostField::Url, .message = std::string { describe(url.error()) } });
    if (auto const scheme = hostSchemeOf(draft.url))
        if (auto const token = checkHostToken(*scheme, draft.tokenFile); !token)
            problems.push_back(
                { .field = HostField::TokenFile, .message = std::string { describe(token.error()) } });
    return problems;
}

HostDraft hostDraftOf(HostEntry const& entry)
{
    return HostDraft {
        .url = toString(entry.url),
        .tokenFile = core::pathToUtf8(entry.tokenFile),
        .tlsCa = core::pathToUtf8(entry.tlsCa),
        .remoteCommand = entry.remoteCommand,
        .profile = entry.profile,
    };
}

std::string toString(HostUrl const& url)
{
    auto text = std::format("{}://", schemeInfo(url.scheme).name);
    if (!url.user.empty())
        text += std::format("{}@", url.user);
    text += url.host.contains(':') ? std::format("[{}]", url.host) : url.host;
    if (url.port)
        text += std::format(":{}", *url.port);
    return text;
}

} // namespace contour::config
