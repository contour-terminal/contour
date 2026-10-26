// SPDX-License-Identifier: Apache-2.0
#include <contour/window/ToastPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>

using contour::window::BarEdge;
using contour::window::BarPresence;
using contour::window::placeToast;
using contour::window::ToastCoveredRows;
using contour::window::ToastHost;
using contour::window::ToastPlacementInputs;

namespace
{

constexpr auto ToastHeight = 36;
constexpr auto BandHeight = 38;
constexpr auto BareBandHeight = 28;

[[nodiscard]] constexpr ToastPlacementInputs inputs(BarPresence band,
                                                    BarEdge edge,
                                                    int bandHeight,
                                                    int cursorRow)
{
    return { .band = band,
             .edge = edge,
             .bandHeight = bandHeight,
             .toastHeight = ToastHeight,
             .cursorRow = cursorRow,
             .coveredRows = ToastCoveredRows };
}

} // namespace

TEST_CASE("placeToast: a band at the top holds it, wherever the cursor is", "[toastplacement]")
{
    for (auto const row: std::views::iota(0, ToastCoveredRows + 2))
    {
        CHECK(placeToast(inputs(BarPresence::Shown, BarEdge::Top, BandHeight, row)) == ToastHost::TitleBand);
        CHECK(placeToast(inputs(BarPresence::Pinned, BarEdge::Top, BandHeight, row)) == ToastHost::TitleBand);
    }
}

TEST_CASE("placeToast: without a band it keeps off the prompt row", "[toastplacement]")
{
    for (auto const row: std::views::iota(0, ToastCoveredRows))
        CHECK(placeToast(inputs(BarPresence::Hidden, BarEdge::Top, 0, row)) == ToastHost::ContentBottom);
    CHECK(placeToast(inputs(BarPresence::Hidden, BarEdge::Top, 0, ToastCoveredRows))
          == ToastHost::ContentTop);
}

TEST_CASE("placeToast: a band that cannot hold it leaves it to the content", "[toastplacement]")
{
    auto const belowCover = ToastCoveredRows + 1;
    // Concealed until the pointer reaches the edge.
    CHECK(placeToast(inputs(BarPresence::RevealOnEdge, BarEdge::Top, BandHeight, belowCover))
          == ToastHost::ContentTop);
    // At the bottom: the toast still goes to the top centre, by the cursor rule.
    CHECK(placeToast(inputs(BarPresence::Shown, BarEdge::Bottom, BandHeight, belowCover))
          == ToastHost::ContentTop);
    CHECK(placeToast(inputs(BarPresence::Shown, BarEdge::Bottom, BandHeight, 0)) == ToastHost::ContentBottom);
    // Minimal's bare band is shorter than a toast.
    CHECK(placeToast(inputs(BarPresence::Shown, BarEdge::Top, BareBandHeight, belowCover))
          == ToastHost::ContentTop);
}

TEST_CASE("placeToast: the band's names read back", "[toastplacement]")
{
    using namespace contour::window;
    for (auto const index: std::views::iota(std::size_t { 0 }, BarPresenceCount))
    {
        auto const presence = static_cast<BarPresence>(index);
        CHECK(barPresenceNamed(barPresenceName(presence)) == presence);
    }
    for (auto const index: std::views::iota(std::size_t { 0 }, BarEdgeCount))
    {
        auto const edge = static_cast<BarEdge>(index);
        CHECK(barEdgeNamed(barEdgeName(edge)) == edge);
    }
    CHECK_FALSE(barPresenceNamed("sideways").has_value());
    CHECK_FALSE(barEdgeNamed("").has_value());
}
