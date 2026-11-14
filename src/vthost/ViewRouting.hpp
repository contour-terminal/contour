// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The pure decisions that route a bell, a clipboard write or a switch request to one window
/// when several windows are attached to the same daemon.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

#include <vthost/ConnectionId.hpp>

namespace vthost
{

/// How long `workspace.switch` waits for the window's SwitchReply before it gives up.
inline constexpr auto SwitchAnswerTimeout = std::chrono::seconds { 30 };

/// What the routing decisions need to know about one attached window.
struct ConnectionView
{
    /// The connection the window speaks over.
    ConnectionId connection;

    /// The workspace the window currently displays.
    uint64_t selected = 0;

    /// The host's monotonically rising counter value at this window's last `Focused` report
    /// (0 = never focused).
    uint64_t focusSerial = 0;
};

namespace detail
{
    /// @return The connection with the highest focus serial among the views accepted by @p accept;
    ///         ties go to the last such view (the most recently attached).
    template <typename Accept>
    [[nodiscard]] constexpr std::optional<ConnectionId> mostRecentlyFocused(
        std::span<ConnectionView const> views, Accept accept)
    {
        ConnectionView const* best = nullptr;
        for (auto const& view: views)
            if (accept(view) && (best == nullptr || view.focusSerial >= best->focusSerial))
                best = &view;
        if (best == nullptr)
            return std::nullopt;
        return best->connection;
    }
} // namespace detail

/// Picks the window that rings or raises for an event no workspace owns.
/// @param views The attached windows, oldest first.
/// @return The window with the highest focus serial; ties (all never focused) go to the last view.
[[nodiscard]] constexpr std::optional<ConnectionId> raiseHolder(std::span<ConnectionView const> views)
{
    return detail::mostRecentlyFocused(views, [](ConnectionView const&) { return true; });
}

/// Picks the window that receives a pane's clipboard write.
/// @param views The attached windows, oldest first.
/// @param paneWorkspace The workspace holding the pane.
/// @return Among the windows selecting @p paneWorkspace the one focused last (ties to the last
///         view), or nullopt when none selects it.
[[nodiscard]] constexpr std::optional<ConnectionId> clipboardTarget(std::span<ConnectionView const> views,
                                                                    uint64_t paneWorkspace)
{
    return detail::mostRecentlyFocused(
        views, [paneWorkspace](ConnectionView const& view) { return view.selected == paneWorkspace; });
}

/// Picks the window a switch request is sent to.
/// @param views The attached windows, oldest first.
/// @param workspace The workspace to switch away from, when the request names one.
/// @return `clipboardTarget` for a named workspace, else `raiseHolder`.
[[nodiscard]] constexpr std::optional<ConnectionId> switchTarget(std::span<ConnectionView const> views,
                                                                 std::optional<uint64_t> workspace)
{
    if (workspace)
        return clipboardTarget(views, *workspace);
    return raiseHolder(views);
}

} // namespace vthost
