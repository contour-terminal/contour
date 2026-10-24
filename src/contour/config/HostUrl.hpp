// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace contour::config
{

/// How a remote host is reached.
enum class HostScheme : std::uint8_t
{
    Ssh, ///< `ssh <target> contour daemon --stdio`; ssh authenticates.
    Tls, ///< A TCP daemon over TLS; its token authenticates.
};

/// Whether a scheme's URL must name a port.
enum class PortRule : std::uint8_t
{
    Optional, ///< The transport has a default port of its own (ssh's 22, or its config).
    Required, ///< No default: a TCP daemon listens wherever `--listen-tcp` said.
};

/// One row of the host URL grammar's scheme table.
struct HostSchemeInfo
{
    std::string_view name; ///< The scheme as written before `://`, lower case.
    HostScheme scheme;     ///< What it selects.
    PortRule port;         ///< Whether a port is required.
};

/// Every scheme a host URL may use. Adding a transport is adding a row.
inline constexpr auto HostSchemes =
    std::array { HostSchemeInfo { "ssh", HostScheme::Ssh, PortRule::Optional },
                 HostSchemeInfo { "tls", HostScheme::Tls, PortRule::Required } };

/// A parsed host URL: `ssh://[user@]host[:port]` or `tls://host:port`.
struct HostUrl
{
    HostScheme scheme = HostScheme::Ssh; ///< How the host is reached.
    std::string user;                    ///< The login (ssh only); empty for ssh's own default.
    std::string host;                    ///< Name or address, an IPv6 literal without its brackets.
    std::optional<std::uint16_t> port;   ///< The port, when the URL names one.
};

/// Why a host URL was refused.
enum class HostUrlError : std::uint8_t
{
    MissingScheme,  ///< No `scheme://` at all.
    UnknownScheme,  ///< A scheme that is not in @ref HostSchemes.
    MissingHost,    ///< Nothing names the host.
    OptionLikeHost, ///< The host begins with '-', which ssh would read as an option.
    OptionLikeUser, ///< The user begins with '-', which ssh would read as an option.
    UnsafeHost,     ///< The host holds a blank, a control character or a shell metacharacter.
    UnsafeUser,     ///< The user holds a control character or a shell metacharacter.
    BadPort,        ///< The port is not a number from 1 to 65535.
    PortRequired,   ///< The scheme needs a port and the URL names none.
    UnexpectedPath, ///< A path follows the authority.
    UserNotAllowed, ///< A user on a scheme that has no login.
};

/// Parses a host URL.
/// @param url The URL, e.g. `ssh://me@homepc:2222` or `tls://studio.tailnet.ts.net:7297`. The scheme is
///            case-insensitive; an IPv6 host is written in brackets.
/// @return The URL's parts, or why it was refused.
[[nodiscard]] std::expected<HostUrl, HostUrlError> parseHostUrl(std::string_view url);

/// @return A human-readable sentence saying what is wrong, for the user who wrote the URL.
[[nodiscard]] std::string_view describe(HostUrlError error) noexcept;

/// @return @p url in its canonical spelling, e.g. `ssh://me@homepc:2222`, an IPv6 host in brackets.
[[nodiscard]] std::string toString(HostUrl const& url);

/// One saved host of the configuration's `hosts:` section.
struct HostEntry
{
    HostUrl url;                     ///< Where the host is.
    std::filesystem::path tokenFile; ///< The daemon's token (`tls://`, where it is required).
    std::filesystem::path tlsCa;     ///< A CA to verify the daemon's certificate against; empty for none.
    std::string remoteCommand;       ///< Replaces the whole remote command (`ssh://`); empty for the default.
    std::string profile; ///< The profile the remote sessions are created with; empty for the default.
};

} // namespace contour::config
