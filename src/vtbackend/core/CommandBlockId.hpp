// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>

#include <boxed-cpp/boxed.hpp>

namespace vtbackend
{

namespace detail
{
    struct CommandBlockIdTag
    {
    };
} // namespace detail

/// The terminal's own monotonic handle for one shell command block; zero means "no block".
///
/// Every row of the primary screen carries one, in the padding after Line::_dirty, so attributing a row to
/// the command that produced it costs no memory per line. The record the id names lives in CommandBlockStore,
/// which outlives the rows: a row whose id no longer resolves simply belongs to no block.
///
/// Its own header in core/, apart from shell/CommandBlock.hpp, for the reason core/ContextId.hpp is one:
/// grid/Line.hpp -- the hottest header in the tree, and below shell/ in the layering -- has to name the
/// type without acquiring the store, the clocks and <deque>.
/// @see Line::blockId
using CommandBlockId = boxed::boxed<uint32_t, detail::CommandBlockIdTag>;

} // namespace vtbackend

template <>
struct std::formatter<vtbackend::CommandBlockId>: formatter<uint32_t> // NOLINT(readability-identifier-naming)
{
    auto format(vtbackend::CommandBlockId id, auto& ctx) const
    {
        return formatter<uint32_t>::format(id.value, ctx);
    }
};
