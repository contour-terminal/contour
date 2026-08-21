// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>

#include <boxed-cpp/boxed.hpp>

namespace vtbackend
{

namespace detail
{
    struct ContextIdTag
    {
    };
} // namespace detail

/// The terminal's own handle on a hierarchical context (OSC 3008), as carried by a grid line.
///
/// Small and dense on purpose: it rides in the two bytes of padding Line already had between its flags
/// and its first column offset, so associating every line with the context that produced it costs
/// nothing per line. Zero means "no context" -- the state a session is in before the first `start=`,
/// and the state a blank line is in.
///
/// The APPLICATION's identifier is not this: the protocol's ids are up to 64 bytes of free-form text,
/// which no grid line can carry. The mapping between the two lives in @ref ContextStack, which is also
/// what keeps a record alive after its context has ended.
///
/// Its own header, separate from TerminalContext.hpp, so that grid/Line.hpp -- the hottest header in
/// the tree -- carries two bytes without acquiring <deque>, <span> and the whole stack model. The same
/// leaf role core/FileUrl.hpp plays for URL decisions.
using ContextId = boxed::boxed<uint16_t, detail::ContextIdTag>;

} // namespace vtbackend

template <>
struct std::formatter<vtbackend::ContextId>: formatter<uint16_t> // NOLINT(readability-identifier-naming)
{
    auto format(vtbackend::ContextId id, auto& ctx) const
    {
        return formatter<uint16_t>::format(id.value, ctx);
    }
};
