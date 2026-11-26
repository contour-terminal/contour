// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/TabAttention.hpp>

#include <cstdint>

namespace contour::window
{

/// What a window saw of a pane whose flags a daemon keeps (FlagSource::Daemon), as it hands it back to the
/// daemon: the serial of each flag it saw, and the newest program-status generation it saw. The window's
/// spelling of vthost::SeenSerials, converted where a daemon is reached.
struct PaneSeen
{
    TabAttention::FlagSerials serials; ///< The serial of each flag seen; 0 acknowledges none.
    uint64_t statusGeneration = 0;     ///< The newest program-status generation seen.

    bool operator==(PaneSeen const&) const = default;
};

/// Whether a window has the user's focus, as it tells the daemon of the panes it shows.
enum class WindowFocusState : uint8_t
{
    Unfocused = 0, ///< Another window, or another application, has it.
    Focused,       ///< The user works in this window.
};

} // namespace contour::window
