// SPDX-License-Identifier: Apache-2.0
#include <crispy/utils.h>

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <vtconformance/MarkerScanner.h>

namespace vtconformance
{

MarkerScanner::MarkerScanner(std::span<std::string_view const> markers): _markers(markers)
{
    auto const lengths = _markers | std::views::transform(&std::string_view::size);
    auto const longest = std::ranges::max_element(lengths);
    _longest = longest != lengths.end() ? *longest : 0;
}

std::vector<MarkerScanner::Match> MarkerScanner::scan(std::string_view chunk)
{
    auto const window = _tail + std::string { chunk };
    auto const tailSize = _tail.size();
    auto matches = std::vector<Match> {};

    for (auto const& [index, marker]: crispy::views::enumerate(_markers))
    {
        // An empty marker matches at every offset and would announce a prompt that is not there.
        if (marker.empty())
            continue;

        auto found = window.find(marker);
        while (found != std::string::npos)
        {
            // Report only what ends inside this chunk. A match ending at or before the seam finished in
            // an earlier chunk and was reported then -- the tail is there to complete a marker that
            // straddles the seam, not to hand the previous one back a second time.
            if (auto const end = found + marker.size(); end > tailSize)
                matches.push_back(Match { .endOffset = end - tailSize, .markerIndex = size_t(index) });

            found = window.find(marker, found + marker.size());
        }
    }

    // In chunk order, so a driver can cut the stream at each in turn; ties broken by table order, which
    // is the same first-match-wins rule the screen matcher used.
    std::ranges::sort(
        matches, {}, [](Match const& match) { return std::pair { match.endOffset, match.markerIndex }; });

    _tail =
        _longest > 1 ? window.substr(window.size() - std::min(window.size(), _longest - 1)) : std::string {};

    return matches;
}

} // namespace vtconformance
