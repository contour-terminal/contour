// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The window's spelling of what it tells a daemon, converted to the wire's: shared by a window's own link
/// (DaemonLink) and the observers of daemons no window of the process is on (DaemonPresence).

#include <contour/window/PaneSeen.hpp>

#include <vthost/proto/Pdu.hpp>

namespace contour::remote
{

/// @return @p focus as the wire states it.
[[nodiscard]] constexpr vthost::proto::WindowFocus wireFocusOf(window::WindowFocusState focus) noexcept
{
    switch (focus)
    {
        case window::WindowFocusState::Unfocused: return vthost::proto::WindowFocus::Unfocused;
        case window::WindowFocusState::Focused: return vthost::proto::WindowFocus::Focused;
    }
    return vthost::proto::WindowFocus::Unfocused;
}

/// @return @p seen as the wire and the daemon's ledger state it.
[[nodiscard]] constexpr vthost::proto::SeenSerials wireSeenOf(window::PaneSeen const& seen) noexcept
{
    return vthost::proto::SeenSerials { .activity = seen.serials.activity,
                                        .bell = seen.serials.bell,
                                        .finished = seen.serials.finished,
                                        .statusGeneration = seen.statusGeneration };
}

} // namespace contour::remote
