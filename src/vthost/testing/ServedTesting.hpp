// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the served-session tests share: a fake extension that opens a served tab and draws a fixed pattern.

#include <vtbackend/core/Primitives.hpp>

#include <optional>
#include <string_view>

#include <vthost/SessionHost.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost::testing
{

/// What the fake extension draws into its tab: the screen cleared, then a word at the top left.
inline constexpr auto ServedPattern = std::string_view { "\x1b[2J\x1b[Hserved-stub" };

/// What a served tab's screen shows once ServedPattern was drawn.
inline constexpr auto ServedText = std::string_view { "served-stub" };

/// The size the fake extension's device starts at.
inline constexpr auto ServedSize =
    vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) };

/// Opens a served tab in @p host's workspace, as an extension does, and draws ServedPattern into it.
/// @param host The host.
/// @return The tab's session; nullopt when the host refused the spawn.
[[nodiscard]] inline std::optional<vtworkspace::SessionId> openServedTab(SessionHost& host)
{
    auto const device = host.served().makeDevice(ServedSize);
    auto const* const tab = host.createTabIn(
        host.workspaceId(),
        { .served =
              ServedSpawn { .extension = "stub", .scheme = "stub", .moduleDigest = "", .device = device } });
    if (tab == nullptr)
        return std::nullopt;
    device->feed(ServedPattern);
    return tab->rootPane()->session();
}

} // namespace vthost::testing
