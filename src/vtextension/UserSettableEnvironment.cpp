// SPDX-License-Identifier: Apache-2.0
#include <vtextension/UserSettableEnvironment.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/RootPointer.hpp>

namespace vtextension
{

namespace
{
    /// The parts of a URL the shape rules judge: its scheme, its authority, and the host the authority names.
    struct UrlParts
    {
        std::string_view scheme;    ///< Before `://`.
        std::string_view authority; ///< After `://`, up to the first `/`.
        std::string_view
            host; ///< The authority without its port; a bracketed IPv6 address keeps its brackets.
    };

    /// @return Whether @p c is printable ASCII, a space excluded: `0x21`-`0x7E`.
    [[nodiscard]] constexpr bool isVisibleAscii(char c) noexcept
    {
        return c >= 0x21 && c <= 0x7E;
    }

    /// @return Whether @p c may appear in a host list item: a host name, a domain, an address or a CIDR
    /// range.
    [[nodiscard]] constexpr bool isHostListChar(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.'
               || c == '_' || c == ':' || c == '/' || c == '[' || c == ']' || c == '-';
    }

    /// @return The host of @p authority, without a port; empty where a `[` has no closing `]`.
    [[nodiscard]] std::string_view hostOf(std::string_view authority) noexcept
    {
        if (authority.starts_with('['))
        {
            auto const close = authority.find(']');
            return close == std::string_view::npos ? std::string_view {} : authority.substr(0, close + 1);
        }
        return authority.substr(0, authority.find(':'));
    }

    /// @return Whether @p port is empty, or `:` and digits: the port a loopback URL may carry.
    [[nodiscard]] constexpr bool isOptionalPort(std::string_view port) noexcept
    {
        return port.empty()
               || (port.size() > 1 && port.front() == ':'
                   && std::ranges::all_of(port.substr(1), [](char c) { return c >= '0' && c <= '9'; }));
    }

    /// Splits a URL of the form `scheme://authority[/…]`, judging the rules both URL shapes share: at most
    /// MaxUserValueBytes, printable ASCII only, and a non-empty host in an authority without `@`.
    /// @param value The URL as configured.
    /// @return Its parts; std::nullopt where the shared rules, or the `://`, are not met.
    [[nodiscard]] std::optional<UrlParts> urlPartsOf(std::string_view value) noexcept
    {
        if (value.size() > MaxUserValueBytes || !std::ranges::all_of(value, isVisibleAscii))
            return std::nullopt;
        auto const separator = value.find("://");
        if (separator == std::string_view::npos)
            return std::nullopt;
        auto const rest = value.substr(separator + 3);
        auto const authority = rest.substr(0, rest.find('/'));
        auto const host = hostOf(authority);
        if (authority.empty() || host.empty() || authority.contains('@'))
            return std::nullopt;
        return UrlParts { .scheme = value.substr(0, separator), .authority = authority, .host = host };
    }

    /// @return Whether the host is one a plain http:// URL may name: the loopback addresses.
    [[nodiscard]] constexpr bool isLoopbackHost(std::string_view host) noexcept
    {
        return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
    }

    /// @return The value unchanged where it is an https:// URL, or an http:// one to the loopback host with
    /// an
    ///         optional port; NotAllowedUrl otherwise.
    [[nodiscard]] std::expected<std::string, UserValueFault> localOrSecureUrl(std::string_view value) noexcept
    {
        auto const parts = urlPartsOf(value);
        if (!parts)
            return std::unexpected(UserValueFault::NotAllowedUrl);
        if (parts->scheme == "https")
            return std::string(value);
        auto const isLoopback = parts->scheme == "http" && isLoopbackHost(parts->host)
                                && isOptionalPort(parts->authority.substr(parts->host.size()));
        if (!isLoopback)
            return std::unexpected(UserValueFault::NotAllowedUrl);
        return std::string(value);
    }

    /// @return The value unchanged where it is an http, https, socks5 or socks5h URL; NotUrl otherwise.
    [[nodiscard]] std::expected<std::string, UserValueFault> proxyUrl(std::string_view value) noexcept
    {
        auto const parts = urlPartsOf(value);
        if (!parts)
            return std::unexpected(UserValueFault::NotUrl);
        auto const isProxyScheme =
            std::ranges::any_of(std::array { "http", "https", "socks5", "socks5h" },
                                [&](std::string_view scheme) { return parts->scheme == scheme; });
        if (!isProxyScheme)
            return std::unexpected(UserValueFault::NotUrl);
        return std::string(value);
    }

    /// @return The items of a host list, each trimmed of ASCII spaces, joined by `,`; NotHostList where an
    /// item
    ///         is empty or has a character outside the host grammar, or the list is too long.
    [[nodiscard]] std::expected<std::string, UserValueFault> hostList(std::string_view value)
    {
        if (value.size() > MaxUserValueBytes)
            return std::unexpected(UserValueFault::NotHostList);
        auto const items =
            std::views::split(value, ',')
            | std::views::transform([](auto part) { return std::string(part.begin(), part.end()); })
            | std::ranges::to<std::vector<std::string>>();
        auto trimmed = std::vector<std::string_view> {};
        trimmed.reserve(items.size());
        for (auto const& item: items)
        {
            auto const first = item.find_first_not_of(' ');
            if (first == std::string::npos)
                return std::unexpected(UserValueFault::NotHostList);
            auto const view = std::string_view(item).substr(first, item.find_last_not_of(' ') - first + 1);
            auto const isWildcard = view == "*";
            if (!isWildcard && !std::ranges::all_of(view, isHostListChar))
                return std::unexpected(UserValueFault::NotHostList);
            trimmed.push_back(view);
        }
        auto joined = std::string {};
        for (auto const item: trimmed)
        {
            if (!joined.empty())
                joined += ',';
            joined += item;
        }
        return joined;
    }

} // namespace

UserSettableVariable const* userSettableRowOf(std::string_view name) noexcept
{
    for (auto const& row: UserSettableVariables)
        if (row.name == name)
            return &row;
    return nullptr;
}

std::expected<std::string, UserValueFault> normalisedUserValue(std::string_view name,
                                                               std::string_view value,
                                                               std::string_view home,
                                                               vtpty::PathStyle style)
{
    auto const* row = userSettableRowOf(name);
    if (row == nullptr)
        return std::unexpected(UserValueFault::NotSettable);
    switch (row->shape)
    {
        case UserValueShape::AbsolutePath:
            return rootSpellingOf(value, home, style).transform_error([](RootPointerError) {
                return UserValueFault::NotAbsolutePath;
            });
        case UserValueShape::LocalOrSecureUrl: return localOrSecureUrl(value);
        case UserValueShape::ProxyUrl: return proxyUrl(value);
        case UserValueShape::HostList: return hostList(value);
    }
    std::unreachable();
}

bool isNormalisedUserValue(std::string_view name, std::string_view value, vtpty::PathStyle style)
{
    return normalisedUserValue(name, value, "", style) == value;
}

std::string_view userValueFaultText(UserValueFault fault) noexcept
{
    return UserValueFaultTexts.at(std::to_underlying(fault)).text;
}

CheckedEnvironmentSets checkEnvironmentSets(std::span<EnvironmentSet const> sets,
                                            std::string_view home,
                                            vtpty::PathStyle style)
{
    auto checked = CheckedEnvironmentSets {};
    for (auto const& set: sets)
    {
        auto& kept = checked.sets.emplace_back(EnvironmentSet { .name = set.name, .variables = {} });
        for (auto const& entry: set.variables)
        {
            auto normalised = normalisedUserValue(entry.name, entry.value, home, style);
            if (normalised)
                kept.variables.push_back(
                    EnvironmentEntry { .name = entry.name, .value = std::move(*normalised) });
            else
                checked.problems.push_back(EnvironmentSetProblem {
                    .set = set.name, .variable = entry.name, .fault = normalised.error() });
        }
    }
    return checked;
}

} // namespace vtextension
