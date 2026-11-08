// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a daemon's restore brought back, as the first client after it is told.

#include <cstdint>

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

} // namespace vthost
