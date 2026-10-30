// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

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

/// Why a saved host whose URL parses still cannot be used.
enum class HostEntryError : std::uint8_t
{
    TokenRequired, ///< A `tls://` host names no token file: a TCP daemon always requires one.
};

/// @return The sentence the loader logs, and Settings shows, for @p error.
[[nodiscard]] std::string_view describe(HostEntryError error) noexcept;

/// Whether a host needs @p tokenFile and has none: what the loader and Settings both refuse beyond the URL.
/// @param scheme    The host's transport.
/// @param tokenFile The token file it names, as written; empty for none.
/// @return Nothing, or why the host cannot be used.
[[nodiscard]] std::expected<void, HostEntryError> checkHostToken(HostScheme scheme,
                                                                 std::string_view tokenFile) noexcept;

/// @return The scheme @p url begins with (`ssh://`, `TLS://`), whether or not the rest of it parses; nullopt
///         while it names none of @ref HostSchemes. What a half-typed address already says about its host.
[[nodiscard]] std::optional<HostScheme> hostSchemeOf(std::string_view url) noexcept;

/// @return The name @p scheme is written with before `://`, lower case: `ssh`.
[[nodiscard]] std::string_view hostSchemeName(HostScheme scheme) noexcept;

/// A field of a saved host, as the `hosts:` entry and its side file spell it.
enum class HostField : std::uint8_t
{
    Url = 0,       ///< `url`
    TokenFile,     ///< `token_file`
    TlsCa,         ///< `tls_ca`
    RemoteCommand, ///< `remote_command`
    Profile,       ///< `profile`
};

/// A saved host as written, field by field: what Settings edits and writes to `hosts/<name>.yml`. Unlike a
/// @ref HostEntry it may not parse yet, and keeps a path as the user typed it (`~/studio.token`).
struct HostDraft
{
    std::string url {};           ///< The address, e.g. `ssh://me@homepc`.
    std::string tokenFile {};     ///< The daemon's token; empty for none.
    std::string tlsCa {};         ///< The CA the daemon's certificate is verified against; empty for none.
    std::string remoteCommand {}; ///< Replaces the remote command (`ssh://`); empty for the default.
    std::string profile {}; ///< The profile the remote sessions are created with; empty for the default.

    [[nodiscard]] bool operator==(HostDraft const&) const = default;
};

/// One field of @ref HostDraft: its key in YAML and where it is kept. Adding a field is adding a row.
struct HostFieldInfo
{
    HostField field;                 ///< The field.
    std::string_view key;            ///< Its key under a host's name, and in its side file.
    std::string HostDraft::* member; ///< Where a draft keeps it.
};

/// Every field of a host, in enumerator order.
inline constexpr auto HostFields = std::array {
    HostFieldInfo { HostField::Url, "url", &HostDraft::url },
    HostFieldInfo { HostField::TokenFile, "token_file", &HostDraft::tokenFile },
    HostFieldInfo { HostField::TlsCa, "tls_ca", &HostDraft::tlsCa },
    HostFieldInfo { HostField::RemoteCommand, "remote_command", &HostDraft::remoteCommand },
    HostFieldInfo { HostField::Profile, "profile", &HostDraft::profile },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, HostFields.size()),
                                  [](std::size_t i) {
                                      return static_cast<std::size_t>(HostFields[i].field) == i;
                                  }),
              "HostFields lists the fields in enumerator order");

/// @return Where a draft keeps @p field.
[[nodiscard]] constexpr std::string HostDraft::* hostDraftMember(HostField field) noexcept
{
    return HostFields[static_cast<std::size_t>(field)].member;
}

/// What is wrong with one field of a draft.
struct HostFieldProblem
{
    HostField field = HostField::Url; ///< The field the problem is shown under.
    std::string message;              ///< What the loader would say about it.

    [[nodiscard]] bool operator==(HostFieldProblem const&) const = default;
};

/// What keeps @p draft from loading, field by field, in the loader's own words: an address that does not
/// parse, and a `tls://` host without a token file. The token is judged by the address's scheme alone, so a
/// half-typed `tls://` address already says it needs one.
/// @param draft The host as written.
/// @return The problems, in @ref HostField order; empty when the loader would take it.
[[nodiscard]] std::vector<HostFieldProblem> checkHostDraft(HostDraft const& draft);

/// @param entry A saved host the loader read.
/// @return It as a draft, its paths as the loader resolved them.
[[nodiscard]] HostDraft hostDraftOf(HostEntry const& entry);

} // namespace contour::config
