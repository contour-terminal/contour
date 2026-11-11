// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace contour::window
{

/// What a window's seen-state is kept under for one pane: the daemon's session id for a session bound to a
/// daemon session, so two windows showing that session each keep their own marks under the same key; the
/// local session id otherwise.
struct PaneKey
{
    uint64_t value = 0; ///< The id the key stands for.

    auto operator<=>(PaneKey const&) const = default;
};

} // namespace contour::window

template <>
struct std::hash<contour::window::PaneKey>
{
    [[nodiscard]] std::size_t operator()(contour::window::PaneKey const& key) const noexcept
    {
        return std::hash<uint64_t> {}(key.value);
    }
};
