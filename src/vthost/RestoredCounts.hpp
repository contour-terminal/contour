// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a daemon's restore brought back, as the first client after it is told.

#include <cstdint>
#include <span>

namespace vthost
{

/// How many workspaces and tabs a workspace restore realized. A workspace counts when at least one of its
/// tabs was created, a tab when at least one of its panes spawned; panes the user closed were never in the
/// snapshot.
struct RestoredCounts
{
    uint32_t workspaces = 0; ///< The workspaces that came back.
    uint32_t tabs = 0;       ///< The tabs that came back, over all workspaces.

    /// @return true where both counts are equal.
    [[nodiscard]] bool operator==(RestoredCounts const&) const = default;
};

/// A workspace a restore brought back, as the announcement of it is held until a client is told.
struct RestoredWorkspace
{
    uint64_t workspace = 0; ///< Its id.
    uint32_t tabs = 0;      ///< The tabs of it with at least one spawned pane.

    /// @return true where both fields are equal.
    [[nodiscard]] bool operator==(RestoredWorkspace const&) const = default;
};

/// @param entries Restored workspaces.
/// @return How many workspaces @p entries names, and their tabs.
[[nodiscard]] constexpr RestoredCounts countRestored(std::span<RestoredWorkspace const> entries) noexcept
{
    auto counts = RestoredCounts {};
    for (auto const& entry: entries)
    {
        ++counts.workspaces;
        counts.tabs += entry.tabs;
    }
    return counts;
}

} // namespace vthost
