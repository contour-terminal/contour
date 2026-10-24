// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `TabPresentation` -- how a tab is presented, as a client and the daemon compare it: the part of a tab's
/// state a user changes without changing what runs in it.

#include <cstdint>
#include <optional>
#include <string>

namespace vthost::client
{

/// Whether a tab is its window's active one.
enum class TabActivity : uint8_t
{
    Inactive = 0,
    Active = 1,
};

/// How one tab is presented: which pane is active, which one is zoomed, its title and colour, and whether it
/// is the window's active tab. Sessions are named by the daemon's ids, the only names both ends share.
struct TabPresentation
{
    uint64_t activeSession = 0;          ///< The session of the active pane; 0 for none.
    uint64_t zoomedSession = 0;          ///< The session of the zoomed pane; 0 when the tab is not zoomed.
    std::optional<std::string> title {}; ///< The runtime title; std::nullopt for the automatic one.
    std::optional<uint32_t> color {};    ///< 0xRRGGBB; std::nullopt for the default colour.
    TabActivity activity = TabActivity::Inactive;
    bool operator==(TabPresentation const&) const = default;
};

} // namespace vthost::client
