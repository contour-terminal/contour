// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How the daemon resolves ONE authoritative client area from the several its attached clients
/// report.
///
/// A hosted session has exactly one grid, and every attached client renders that same grid. When
/// two clients of different sizes are attached, something has to decide which size the grid is —
/// and until this existed the answer was "whichever client reported last", which is not a policy
/// but a race: two differently-sized clients took turns resizing every application on the daemon.
///
/// The resolution is a PURE function of the reported set, which is what makes the result stable:
/// re-reporting the same areas in any order yields the same answer, so the system settles instead
/// of oscillating. It is also why this is a dependency-free header — the decision is testable
/// without a daemon, a socket or a terminal.

#include <vtpty/PageSize.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace vthost
{

/// Which attached client's size wins when they disagree.
///
/// The names and the default match tmux's `window-size` option, because the situation is the same
/// one and users arrive with that vocabulary. tmux's fourth value, `manual`, is deliberately not
/// offered: it needs a runtime verb to set the size with, and nothing in this daemon's protocol or
/// UI would drive one.
enum class ClientSizePolicy : uint8_t
{
    /// The size of the client that reported most recently. tmux's default, and the least
    /// surprising: the window you just resized is the one you are looking at. Other clients see a
    /// grid that is not their size and letterbox or pan it.
    Latest,

    /// The largest area every client can fully display — the intersection. No client is ever asked
    /// to show a grid bigger than itself, at the cost of a small client shrinking everyone.
    Smallest,

    /// The union: every client can see everything any client can see. Maximizes the space
    /// applications get, and every smaller client pans.
    Largest,
};

/// The policy's spellings, single-sourced: the config parser, the log line and any diagnostic all
/// read this one table, so a fourth policy is a row rather than three edits.
constexpr auto ClientSizePolicyNames = std::to_array<std::pair<std::string_view, ClientSizePolicy>>({
    { "latest", ClientSizePolicy::Latest },
    { "smallest", ClientSizePolicy::Smallest },
    { "largest", ClientSizePolicy::Largest },
});

/// @param policy The policy to name.
/// @return Its configuration spelling.
[[nodiscard]] constexpr std::string_view nameOf(ClientSizePolicy policy) noexcept
{
    for (auto const& [name, value]: ClientSizePolicyNames)
        if (value == policy)
            return name;
    return "latest";
}

/// @param name A configuration spelling, as written by a user.
/// @return The policy it names, or nullopt if it names none — so a caller can report the typo
///         rather than silently substituting a default the user did not ask for.
[[nodiscard]] constexpr std::optional<ClientSizePolicy> clientSizePolicyFrom(std::string_view name) noexcept
{
    for (auto const& [candidate, value]: ClientSizePolicyNames)
        if (candidate == name)
            return value;
    return std::nullopt;
}

/// One client's reported area, with the order it was reported in.
struct ClientArea
{
    vtpty::PageSize size {};
    /// A monotonically increasing stamp, so `Latest` has something to compare. Not a timestamp:
    /// the resolution must be reproducible in a test without a clock.
    uint64_t sequence = 0;
};

/// Resolves the authoritative client area.
///
/// @param policy Which client's size wins.
/// @param areas Every attached client's reported area, in any order. A range rather than a span so
///        a caller holding them in a map can pass a view of its values without copying them out —
///        this runs on every reported area, and a window drag reports one per frame.
/// @return The area to project the pane trees into, or nullopt when no client has reported one —
///         in which case the caller keeps whatever it had, since a daemon with no attached client
///         still hosts sessions that must stay some size.
[[nodiscard]] inline std::optional<vtpty::PageSize> resolveClientArea(ClientSizePolicy policy,
                                                                      std::ranges::forward_range auto&& areas)
{
    if (std::ranges::empty(areas))
        return std::nullopt;

    switch (policy)
    {
        case ClientSizePolicy::Latest: {
            // Dereferenced rather than arrowed: a view's iterator (a map's `values`, say) need not
            // provide operator->.
            auto const latest = std::ranges::max_element(areas, {}, &ClientArea::sequence);
            return (*latest).size;
        }
        case ClientSizePolicy::Smallest:
        case ClientSizePolicy::Largest: {
            // Per axis, not per client: two clients can each be the wider and the shorter one, and
            // picking a single client's area would then hand the other a grid it cannot show. tmux
            // combines the axes independently for the same reason.
            auto const lines =
                areas | std::views::transform([](ClientArea const& a) { return a.size.lines; });
            auto const columns =
                areas | std::views::transform([](ClientArea const& a) { return a.size.columns; });
            if (policy == ClientSizePolicy::Smallest)
                return vtpty::PageSize { .lines = std::ranges::min(lines),
                                         .columns = std::ranges::min(columns) };
            return vtpty::PageSize { .lines = std::ranges::max(lines), .columns = std::ranges::max(columns) };
        }
    }
    return std::nullopt;
}

} // namespace vthost
