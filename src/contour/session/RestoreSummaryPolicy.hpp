// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace contour::session
{

/// Whether the boot window has adopted the daemon's primary window yet.
enum class AttachBoot : uint8_t
{
    Pending = 0, ///< Not yet: the next daemon window shown goes into the boot window.
    Adopted = 1, ///< It has: every later daemon window gets an OS window of its own.
};

/// Whether the daemon's layout names any window.
enum class LayoutWindows : uint8_t
{
    None = 0, ///< No layout arrived yet, or it is empty: nothing has been realized.
    Some,     ///< At least one daemon window is to be shown.
};

/// Whether an OS window is being spawned for a daemon window right now.
enum class AttachSpawn : uint8_t
{
    Idle = 0, ///< None is.
    Spawning, ///< One is: it binds itself (and gets the summary) once its QML has loaded.
};

/// What becomes of the held restore summary once a reconciliation pass has run.
enum class RestoreSummaryFate : uint8_t
{
    Keep = 0, ///< A window is still to be bound: the summary goes there.
    Expire,   ///< Every window of the layout has had its turn: a later window never gets the summary.
};

/// Decides whether the pending restore summary survives the reconciliation pass that just ran.
/// A window still to be bound -- the boot window is not up yet, or one is being spawned -- is where the
/// summary goes; and without a layout nothing has been realized, so nothing is yet missed.
/// @param layout Whether the daemon's layout names any window.
/// @param spawn Whether an OS window is being spawned for a daemon window.
/// @param boot Whether the boot window has adopted the primary daemon window.
/// @return Keep while a window may still receive the summary, else Expire.
[[nodiscard]] constexpr RestoreSummaryFate restoreSummaryFateAfterLayout(LayoutWindows layout,
                                                                         AttachSpawn spawn,
                                                                         AttachBoot boot) noexcept
{
    if (layout == LayoutWindows::None || spawn == AttachSpawn::Spawning || boot == AttachBoot::Pending)
        return RestoreSummaryFate::Keep;
    return RestoreSummaryFate::Expire;
}

} // namespace contour::session
