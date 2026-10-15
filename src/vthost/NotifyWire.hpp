// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A desktop notification's occasion (OSC 99 `o=`) as it travels on the native protocol -- single-sourced
/// so the server's capture (NativeSession) and the client's apply (ScreenMirror) cannot disagree, as
/// @ref vthost/ProgressWire.hpp does for the progress indicator.
///
/// Decoding is TOTAL: the byte arrives as a peer chose it, and one no row names reads as unstated, which
/// the client decides by its most conservative rule rather than by a guess.

#include <vtbackend/vt/DesktopNotification.hpp>

#include <algorithm>
#include <array>
#include <optional>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// One occasion as the terminal names it and as the wire spells it.
struct NotifyOccasionRow
{
    vtbackend::DisplayOccasion display; ///< The occasion an OSC 99 notification stated.
    proto::NotifyOccasion wire;         ///< Its spelling on the wire.
};

/// Every occasion an OSC 99 notification can state. Unstated has no row: it is the absence of one.
inline constexpr auto NotifyOccasionRows = std::array {
    NotifyOccasionRow { .display = vtbackend::DisplayOccasion::Always,
                        .wire = proto::NotifyOccasion::Always },
    NotifyOccasionRow { .display = vtbackend::DisplayOccasion::Unfocused,
                        .wire = proto::NotifyOccasion::Unfocused },
    NotifyOccasionRow { .display = vtbackend::DisplayOccasion::Invisible,
                        .wire = proto::NotifyOccasion::Invisible },
};

/// @param occasion The occasion a notification stated, or nullopt for one that stated none (OSC 9 / 777).
/// @return Its spelling on the wire.
[[nodiscard]] constexpr proto::NotifyOccasion wireOccasionOf(
    std::optional<vtbackend::DisplayOccasion> occasion) noexcept
{
    if (!occasion)
        return proto::NotifyOccasion::Unstated;
    auto const row = std::ranges::find(NotifyOccasionRows, *occasion, &NotifyOccasionRow::display);
    return row != NotifyOccasionRows.end() ? row->wire : proto::NotifyOccasion::Unstated;
}

/// @param occasion An occasion as the wire spells it.
/// @return The occasion it names, or nullopt for Unstated and for a value no row names.
[[nodiscard]] constexpr std::optional<vtbackend::DisplayOccasion> displayOccasionOf(
    proto::NotifyOccasion occasion) noexcept
{
    auto const row = std::ranges::find(NotifyOccasionRows, occasion, &NotifyOccasionRow::wire);
    return row != NotifyOccasionRows.end() ? std::optional { row->display } : std::nullopt;
}

} // namespace vthost
