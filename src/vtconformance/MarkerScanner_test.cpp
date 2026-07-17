// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtconformance/MarkerScanner.h>

using namespace std::string_view_literals;
using vtconformance::MarkerScanner;

namespace
{

constexpr auto Banner = "Push <RETURN>"sv;
constexpr auto OneMarker = std::array { Banner };

/// @return The marker indices reported for @p chunk, in order.
[[nodiscard]] std::vector<size_t> indicesOf(MarkerScanner& scanner, std::string_view chunk)
{
    auto indices = std::vector<size_t> {};
    for (auto const& match: scanner.scan(chunk))
        indices.push_back(match.markerIndex);
    return indices;
}

} // namespace

TEST_CASE("MarkerScanner.finds a marker inside one chunk", "[vtconformance]")
{
    auto scanner = MarkerScanner { OneMarker };
    auto const matches = scanner.scan("drawing...Push <RETURN>");

    REQUIRE(matches.size() == 1);
    CHECK(matches[0].markerIndex == 0);

    // The cut lands one past the banner's last byte: everything before it is the screen the banner
    // announces, everything after belongs to whatever the program does next.
    CHECK(matches[0].endOffset == std::string_view("drawing...Push <RETURN>").size());
}

TEST_CASE("MarkerScanner.reports nothing when the marker is absent", "[vtconformance]")
{
    auto scanner = MarkerScanner { OneMarker };
    CHECK(scanner.scan("nothing to see here").empty());
}

TEST_CASE("MarkerScanner.finds a marker split across a seam, at every offset", "[vtconformance]")
{
    // The whole reason the scanner keeps a tail. A PTY read boundary can fall anywhere, so exhaust it:
    // cut the banner at each of its interior positions and require the match either way.
    for (auto const split: std::views::iota(size_t { 1 }, Banner.size()))
    {
        auto scanner = MarkerScanner { OneMarker };

        auto const first = Banner.substr(0, split);
        auto const second = Banner.substr(split);

        INFO("split at " << split);
        CHECK(scanner.scan(first).empty());

        auto const matches = scanner.scan(second);
        REQUIRE(matches.size() == 1);
        CHECK(matches[0].markerIndex == 0);

        // Reported in the SECOND chunk's coordinates: the bytes of the banner that arrived earlier are
        // already fed, so the cut is what remains of it in this chunk.
        CHECK(matches[0].endOffset == second.size());
    }
}

TEST_CASE("MarkerScanner.finds a marker fed one byte at a time", "[vtconformance]")
{
    auto scanner = MarkerScanner { OneMarker };
    auto found = size_t { 0 };

    for (auto const ch: Banner)
        found += scanner.scan(std::string_view { &ch, 1 }).size();

    CHECK(found == 1);
}

TEST_CASE("MarkerScanner.does not report a marker twice", "[vtconformance]")
{
    auto scanner = MarkerScanner { OneMarker };
    REQUIRE(scanner.scan("Push <RETURN>").size() == 1);

    // The tail still holds the banner's last bytes; they must not be re-matched against what follows.
    CHECK(scanner.scan("\nnext test").empty());
}

TEST_CASE("MarkerScanner.finds back-to-back markers in one chunk", "[vtconformance]")
{
    auto scanner = MarkerScanner { OneMarker };
    auto const matches = scanner.scan("Push <RETURN>\nPush <RETURN>");

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].endOffset < matches[1].endOffset);
}

TEST_CASE("MarkerScanner.reports matches in chunk order", "[vtconformance]")
{
    // Table order is deliberately the reverse of stream order, so a scanner that returned table order
    // would fail here. A driver cuts the stream at each match in turn, so the order IS the contract.
    constexpr auto Markers = std::array { "second"sv, "first"sv };
    auto scanner = MarkerScanner { Markers };

    CHECK(indicesOf(scanner, "first then second") == std::vector<size_t> { 1, 0 });
}

TEST_CASE("MarkerScanner.a shorter marker still straddles a seam", "[vtconformance]")
{
    // The tail is sized by the LONGEST marker, so a short one has more than enough history. It must not
    // be re-reported once it has passed, though -- that is what the seam rule is for.
    constexpr auto Markers = std::array { "a very long marker indeed"sv, "xy"sv };
    auto scanner = MarkerScanner { Markers };

    CHECK(scanner.scan("...x").empty());

    auto const matches = scanner.scan("y...");
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].markerIndex == 1);
    CHECK(matches[0].endOffset == 1);

    // Still inside the (long) tail, but finished before this chunk began.
    CHECK(scanner.scan("z").empty());
}

TEST_CASE("MarkerScanner.ignores an empty marker", "[vtconformance]")
{
    // An empty marker matches at every offset, so it would announce a prompt at every byte.
    constexpr auto Markers = std::array { ""sv, "real"sv };
    auto scanner = MarkerScanner { Markers };

    CHECK(indicesOf(scanner, "a real thing") == std::vector<size_t> { 1 });
}

TEST_CASE("MarkerScanner.tolerates an empty marker table", "[vtconformance]")
{
    auto scanner = MarkerScanner { std::span<std::string_view const> {} };
    CHECK(scanner.scan("anything at all").empty());
    CHECK(scanner.tail().empty());
}

TEST_CASE("MarkerScanner.retains only what a straddle could need", "[vtconformance]")
{
    // The tail is a scanning artifact, not a feeding delay -- but it still must not grow without bound
    // on a long run. One byte short of the longest marker is exactly enough. @see MarkerScanner::_tail.
    auto scanner = MarkerScanner { OneMarker };
    (void) scanner.scan(std::string(64 * 1024, 'x'));

    CHECK(scanner.tail().size() == Banner.size() - 1);
}
