// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the host chip of a remote window says: the supervised connection's six states fold onto the chip's
/// three, each with its dot colour and text. Pure, so the chip, its accessibility name and the tests read
/// the same rows.

#include <contour/config/UiStyle.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

#include <vthost/client/ConnectionState.hpp>

namespace contour::window
{

/// The three states the host chip shows.
enum class HostChipState : uint8_t
{
    Connected = 0, ///< The daemon is reachable.
    Reconnecting,  ///< A connection is being (re)established.
    Disconnected,  ///< Nothing is retried.
};

/// Number of @c HostChipState enumerators, derived from the last one.
inline constexpr size_t HostChipStateCount = static_cast<size_t>(HostChipState::Disconnected) + 1;

/// One row of the connection-to-chip table.
struct HostChipRow
{
    vthost::client::ConnectionState connection; ///< The connection state.
    HostChipState state;                        ///< What the chip shows for it.
};

/// The chip state of every connection state, in enumerator order.
inline constexpr std::array<HostChipRow, vthost::client::ConnectionStateCount> HostChipRows {
    HostChipRow { vthost::client::ConnectionState::Connecting, HostChipState::Reconnecting },
    HostChipRow { vthost::client::ConnectionState::Connected, HostChipState::Connected },
    HostChipRow { vthost::client::ConnectionState::Reconnecting, HostChipState::Reconnecting },
    HostChipRow { vthost::client::ConnectionState::Failed, HostChipState::Disconnected },
    HostChipRow { vthost::client::ConnectionState::Refused, HostChipState::Disconnected },
    HostChipRow { vthost::client::ConnectionState::Stopped, HostChipState::Disconnected },
};

namespace detail
{
    [[nodiscard]] constexpr bool hostChipRowsInOrder() noexcept
    {
        for (auto const i: std::views::iota(size_t { 0 }, HostChipRows.size()))
            if (HostChipRows[i].connection != vthost::client::ConnectionState(i))
                return false;
        return true;
    }
} // namespace detail

static_assert(detail::hostChipRowsInOrder(), "HostChipRows must list the connection states in order");

/// Folds a connection state onto the chip's states.
/// @param connection Where the supervised connection stands.
/// @return Its row in @c HostChipRows.
[[nodiscard]] constexpr HostChipState hostChipStateOf(vthost::client::ConnectionState connection) noexcept
{
    return HostChipRows[static_cast<size_t>(connection)].state;
}

/// Whether a window shows a host chip.
enum class HostChipPresence : uint8_t
{
    Hidden = 0, ///< A local window.
    Shown,      ///< A window onto a remote host.
};

/// Decides whether the chip shows.
/// @param hostLabel The window's host label; empty for a local window.
/// @return Shown when there is a host to name.
[[nodiscard]] constexpr HostChipPresence hostChipPresence(std::string_view hostLabel) noexcept
{
    return hostLabel.empty() ? HostChipPresence::Hidden : HostChipPresence::Shown;
}

/// How one chip state looks.
struct HostChipLook
{
    HostChipState state;              ///< The state this row describes.
    config::StatusColorRole dot;      ///< The dot's status role.
    std::string_view status;          ///< The label after the host name; empty when connected.
    std::string_view accessibleState; ///< The state word assistive technology announces.
};

/// The looks, indexed by @c HostChipState.
inline constexpr std::array<HostChipLook, HostChipStateCount> HostChipLooks {
    HostChipLook { HostChipState::Connected, config::StatusColorRole::Success, "", "connected" },
    HostChipLook {
        HostChipState::Reconnecting, config::StatusColorRole::Warning, "Reconnecting…", "reconnecting" },
    HostChipLook {
        HostChipState::Disconnected, config::StatusColorRole::Danger, "Disconnected", "disconnected" },
};

namespace detail
{
    [[nodiscard]] constexpr bool hostChipLooksInOrder() noexcept
    {
        for (auto const i: std::views::iota(size_t { 0 }, HostChipLooks.size()))
            if (HostChipLooks[i].state != HostChipState(i))
                return false;
        return true;
    }
} // namespace detail

static_assert(detail::hostChipLooksInOrder(), "HostChipLooks must list the states in enumerator order");

/// The look of a chip state.
/// @param state The state.
/// @return Its row in @c HostChipLooks.
[[nodiscard]] constexpr HostChipLook const& hostChipLook(HostChipState state) noexcept
{
    return HostChipLooks[static_cast<size_t>(state)];
}

} // namespace contour::window
