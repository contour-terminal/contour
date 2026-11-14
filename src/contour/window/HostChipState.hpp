// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the location chip says of its daemon's connection: the supervised connection's six states fold onto
/// the chip's four, each with its dot colour and text. Pure, so the chip, its accessibility name and the
/// tests read the same rows.

#include <contour/config/UiStyle.hpp>
#include <contour/window/TranslateNoop.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

#include <vthost/client/ConnectionState.hpp>

namespace contour::window
{

/// The states the location chip shows of its daemon's connection.
enum class HostChipState : uint8_t
{
    Connected = 0, ///< The daemon is reachable.
    Reconnecting,  ///< A connection is being (re)established.
    Disconnected,  ///< Nothing is retried.
    /// A reconnect is held until a window of the user's is exposed: it may ask them something.
    NeedsAttention,
};

/// Number of @c HostChipState enumerators, derived from the last one.
inline constexpr size_t HostChipStateCount = static_cast<size_t>(HostChipState::NeedsAttention) + 1;

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
    // The same daemon comes back: on the chip, a reconnect like any other.
    HostChipRow { vthost::client::ConnectionState::Upgrading, HostChipState::Reconnecting },
    HostChipRow { vthost::client::ConnectionState::NeedsAttention, HostChipState::NeedsAttention },
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

/// @param state A chip state.
/// @return Whether the connection is being sought again, whether or not it waits for the user first.
[[nodiscard]] constexpr bool isSeekingConnection(HostChipState state) noexcept
{
    return state == HostChipState::Reconnecting || state == HostChipState::NeedsAttention;
}

/// Folds a connection state onto the chip's states.
/// @param connection Where the supervised connection stands.
/// @return Its row in @c HostChipRows.
[[nodiscard]] constexpr HostChipState hostChipStateOf(vthost::client::ConnectionState connection) noexcept
{
    return HostChipRows[static_cast<size_t>(connection)].state;
}

/// Whether a window is on a remote host, whose connection the band must be able to report.
enum class HostChipPresence : uint8_t
{
    Hidden = 0, ///< A local window.
    Shown,      ///< A window onto a remote host.
};

/// Decides whether the window names a remote host.
/// @param hostLabel The window's host label; empty for a local window.
/// @return Shown when there is a host to name.
[[nodiscard]] constexpr HostChipPresence hostChipPresence(std::string_view hostLabel) noexcept
{
    return hostLabel.empty() ? HostChipPresence::Hidden : HostChipPresence::Shown;
}

/// Whether the window's connection has news the band must carry, whatever the tab-bar gate says.
enum class HostNotice : uint8_t
{
    None = 0,    ///< A local window, or a remote one whose connection is up.
    Interrupted, ///< A remote window reconnecting or disconnected: the band must be on screen to say so.
};

/// Number of @c HostNotice enumerators, derived from the last one.
inline constexpr size_t HostNoticeCount = static_cast<size_t>(HostNotice::Interrupted) + 1;

/// Decides whether the band must show for the chip's sake.
/// @param presence Whether the window shows a chip at all (@c hostChipPresence).
/// @param state    What the chip shows.
/// @return Interrupted for a chip that shows a connection other than Connected.
[[nodiscard]] constexpr HostNotice hostNoticeOf(HostChipPresence presence, HostChipState state) noexcept
{
    return presence == HostChipPresence::Shown && state != HostChipState::Connected ? HostNotice::Interrupted
                                                                                    : HostNotice::None;
}

/// How one chip state looks and what it says.
struct HostChipLook
{
    HostChipState state;              ///< The state this row describes.
    config::StatusColorRole dot;      ///< The dot's status role.
    std::string_view status;          ///< The label after the host name; empty when connected.
    std::string_view accessibleState; ///< The state word assistive technology announces.
    std::string_view tooltipFormat; ///< The chip's tooltip sentence (`%1` the daemon, `%2` the action); empty
                                    ///< when connected, whose tooltip is the action alone.
    std::string_view description;   ///< What a screen reader adds to the chip's name; empty when connected.
    std::string_view glyph;         ///< The terminal-style stand-in for the dot.
};

/// The looks, indexed by @c HostChipState.
inline constexpr std::array<HostChipLook, HostChipStateCount> HostChipLooks {
    HostChipLook { .state = HostChipState::Connected,
                   .dot = config::StatusColorRole::Success,
                   .status = "",
                   .accessibleState = "connected",
                   .tooltipFormat = "",
                   .description = "",
                   .glyph = "●" },
    HostChipLook { .state = HostChipState::Reconnecting,
                   .dot = config::StatusColorRole::Warning,
                   .status = QT_TRANSLATE_NOOP("LocationChip", "Reconnecting…"),
                   .accessibleState = "reconnecting",
                   .tooltipFormat = QT_TRANSLATE_NOOP("LocationChip", "Reconnecting to %1 · %2"),
                   .description = QT_TRANSLATE_NOOP("LocationChip", "Reconnecting"),
                   .glyph = "◑" },
    HostChipLook { .state = HostChipState::Disconnected,
                   .dot = config::StatusColorRole::Danger,
                   .status = QT_TRANSLATE_NOOP("LocationChip", "Disconnected"),
                   .accessibleState = "disconnected",
                   .tooltipFormat = QT_TRANSLATE_NOOP("LocationChip", "Disconnected from %1 · %2"),
                   .description = QT_TRANSLATE_NOOP("LocationChip", "Disconnected"),
                   .glyph = "✕" },
    HostChipLook { .state = HostChipState::NeedsAttention,
                   .dot = config::StatusColorRole::Warning,
                   .status = QT_TRANSLATE_NOOP("LocationChip", "Reconnecting needs your attention"),
                   .accessibleState = "reconnecting, needs your attention",
                   .tooltipFormat =
                       QT_TRANSLATE_NOOP("LocationChip", "Reconnecting to %1 needs your attention · %2"),
                   .description = QT_TRANSLATE_NOOP("LocationChip", "Reconnecting, needs your attention"),
                   .glyph = "!" },
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
