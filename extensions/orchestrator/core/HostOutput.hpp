// SPDX-License-Identifier: Apache-2.0
#pragma once

// Test support: what Contour's host hands the guest of what a declared command printed.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/Model.hpp>

namespace orchestrator
{

/// @return The most bytes the host keeps of one stream of a declared command, as the contract names it
///         (contract/host-limits/stream-cap.txt, which Contour's test compares with the host's own).
[[nodiscard]] inline std::size_t hostStreamCap()
{
    auto const text = readFixture(FixtureRoot::Contract, "host-limits/stream-cap.txt");
    auto cap = std::size_t {};
    auto const parsed = std::from_chars(text.data(), text.data() + text.size(), cap);
    REQUIRE(parsed.ec == std::errc {});
    return cap;
}

/// What the host keeps of one stream.
struct HostOutput
{
    std::string text;    ///< The bytes kept.
    OutputExtent extent; ///< Whether they are all that was kept of what the command printed.
};

/// What the host keeps of what @p command printed, as vtpty's StreamKeeper does: each line that begins with
/// one of the command's declared dropped prefixes goes, and of the rest at most hostStreamCap() bytes are
/// kept.
/// @param command A declared command's name.
/// @param printed What it printed to its standard output.
/// @return What the guest is handed.
[[nodiscard]] inline HostOutput keptByHost(std::string_view command, std::string_view printed)
{
    auto const* const declared = findCommand(command);
    auto kept = std::string {};
    for (auto const part: std::views::split(printed, '\n'))
    {
        auto const line = std::string_view(part.begin(), part.end());
        // A last part is what followed the last line end: empty when the output ended with one.
        auto const isLast = line.data() + line.size() == printed.data() + printed.size();
        if (isLast && line.empty())
            break;
        auto const dropped = declared != nullptr
                             && std::ranges::any_of(declared->droppedLines,
                                                    [line](auto prefix) { return line.starts_with(prefix); });
        if (dropped)
            continue;
        kept += line;
        if (!isLast)
            kept += '\n';
    }
    auto const cap = hostStreamCap();
    if (kept.size() <= cap)
        return HostOutput { .text = std::move(kept), .extent = OutputExtent::Whole };
    kept.resize(cap);
    return HostOutput { .text = std::move(kept), .extent = OutputExtent::Truncated };
}

/// A `git ls-files -v --stage` listing of @p files plain files with generic names, one of every hundred a
/// symbolic link, and @p extra lines after the first half of them.
/// @param files How many plain entries.
/// @param extra Whole lines, such as a flagged entry, placed midway.
[[nodiscard]] inline std::string indexListing(std::size_t files, std::string_view extra = {})
{
    auto listing = std::string {};
    for (auto const index: std::views::iota(std::size_t { 0 }, files))
    {
        auto const mode = index % 100 == 99 ? std::string_view { "120000" } : std::string_view { "100644" };
        listing += std::format("H {} 78981922613b2afb6025042ff6bd878ac1994e85 0\tmodule-{}/src/file-{}.txt\n",
                               mode,
                               index % 97,
                               index);
        if (index == files / 2)
            listing += extra;
    }
    return listing;
}

} // namespace orchestrator
