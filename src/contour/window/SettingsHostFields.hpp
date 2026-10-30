// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The rows of a saved host's Settings page, as one table: the field each edits, the group it is shown in,
/// the transport it is shown for, and what it reads. Qt-free.

#include <contour/config/HostUrl.hpp>
#include <contour/window/SettingsControls.hpp>
#include <contour/window/SettingsSections.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>

namespace contour::window
{

/// A row of a host's page.
enum class HostPageRow : uint8_t
{
    Address = 0,
    Transport,
    RemoteCommand,
    TokenFile,
    TlsCa,
    Profile,
};

/// How many rows a host's page has.
inline constexpr std::size_t HostPageRowCount = static_cast<std::size_t>(HostPageRow::Profile) + 1;

/// One row of a host's page.
struct HostPageRowInfo
{
    HostPageRow row = HostPageRow::Address;               ///< The row.
    std::string_view key {};                              ///< Its key: the field's YAML key, or its own.
    std::string_view label {};                            ///< What the row is called.
    std::string_view help {};                             ///< Its help line.
    std::string_view placeholder {};                      ///< What an empty field shows; empty for none.
    SettingsGroup group = SettingsGroup::HostConnection;  ///< The group it is shown in.
    SettingsControl control = SettingsControl::TextField; ///< What edits it.
    std::optional<config::HostField> field {};            ///< What it edits; none for a derived row.
    std::optional<config::HostScheme> onlyFor {};         ///< The transport it is shown for; none for all.
};

/// Every row, in page order. Adding a row is adding an enumerator and a row here.
inline constexpr auto HostPageRows = std::array {
    HostPageRowInfo { .row = HostPageRow::Address,
                      .key = "url",
                      .label = "Address",
                      .help = "ssh://[user@]host[:port], or tls://host:port",
                      .placeholder = "ssh://host",
                      .group = SettingsGroup::HostConnection,
                      .control = SettingsControl::TextField,
                      .field = config::HostField::Url,
                      .onlyFor = std::nullopt },
    HostPageRowInfo { .row = HostPageRow::Transport,
                      .key = "transport",
                      .label = "Transport",
                      .help = "Read from the address.",
                      .placeholder = {},
                      .group = SettingsGroup::HostConnection,
                      .control = SettingsControl::Readout,
                      .field = std::nullopt,
                      .onlyFor = std::nullopt },
    HostPageRowInfo { .row = HostPageRow::RemoteCommand,
                      .key = "remote_command",
                      .label = "Remote command",
                      .help =
                          "Replaces the whole command run on the host. Empty runs Contour's daemon from the "
                          "PATH.",
                      .placeholder = "contour daemon --stdio",
                      .group = SettingsGroup::HostSsh,
                      .control = SettingsControl::TextField,
                      .field = config::HostField::RemoteCommand,
                      .onlyFor = config::HostScheme::Ssh },
    HostPageRowInfo { .row = HostPageRow::TokenFile,
                      .key = "token_file",
                      .label = "Token file",
                      .help = "Required. The daemon's token, from contour daemon --listen-tcp.",
                      .placeholder = {},
                      .group = SettingsGroup::HostTls,
                      .control = SettingsControl::PathField,
                      .field = config::HostField::TokenFile,
                      .onlyFor = config::HostScheme::Tls },
    HostPageRowInfo { .row = HostPageRow::TlsCa,
                      .key = "tls_ca",
                      .label = "CA certificate",
                      .help = "Optional. Verifies the daemon's certificate against this CA.",
                      .placeholder = "System trust",
                      .group = SettingsGroup::HostTls,
                      .control = SettingsControl::PathField,
                      .field = config::HostField::TlsCa,
                      .onlyFor = config::HostScheme::Tls },
    HostPageRowInfo { .row = HostPageRow::Profile,
                      .key = "profile",
                      .label = "Profile",
                      .help = "The profile remote sessions are created with: its shell runs on the host.",
                      .placeholder = {},
                      .group = SettingsGroup::HostSessions,
                      .control = SettingsControl::PopUp,
                      .field = config::HostField::Profile,
                      .onlyFor = std::nullopt },
};

static_assert(HostPageRows.size() == HostPageRowCount, "one row per HostPageRow");
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, HostPageRowCount),
                                  [](std::size_t i) {
                                      return static_cast<std::size_t>(HostPageRows[i].row) == i;
                                  }),
              "HostPageRows lists the rows in enumerator order");
static_assert(std::ranges::all_of(HostPageRows,
                                  [](HostPageRowInfo const& row) {
                                      return settingsGroupRow(row.group).scope == SettingsScope::Host;
                                  }),
              "every row of a host's page is in a group of the Hosts section");

/// Whether a host page shows @p row while the address names @p scheme.
/// @param row    A row.
/// @param scheme The transport the address names; none while it names none, which shows only the rows every
///               transport has.
[[nodiscard]] constexpr bool hostPageShows(HostPageRowInfo const& row,
                                           std::optional<config::HostScheme> scheme) noexcept
{
    return !row.onlyFor || row.onlyFor == scheme;
}

/// What the Transport row shows for @p scheme: its name in capitals, as the sidebar's tag reads.
[[nodiscard]] constexpr std::string_view transportLabel(config::HostScheme scheme) noexcept
{
    switch (scheme)
    {
        case config::HostScheme::Ssh: return "SSH";
        case config::HostScheme::Tls: return "TLS";
    }
    return "SSH";
}

} // namespace contour::window
