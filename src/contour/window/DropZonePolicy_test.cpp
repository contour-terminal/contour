// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DropZonePolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>

using contour::window::DropEdgeFraction;
using contour::window::DropExtent;
using contour::window::dropGhostOf;
using contour::window::DropOrigin;
using contour::window::DropPayload;
using contour::window::DropPoint;
using contour::window::DropRect;
using contour::window::DropZone;
using contour::window::DropZoneCount;
using contour::window::DropZoneInputs;
using contour::window::dropZoneRowOf;
using contour::window::DropZoneRows;
using contour::window::resolveDropZone;
using contour::window::SplitSupport;
using vtworkspace::SplitSide;

namespace
{
constexpr auto Pane = DropExtent { .width = 1000, .height = 500 };

constexpr DropZoneInputs tab(double x, double y)
{
    return DropZoneInputs { .payload = DropPayload::Tab,
                            .pane = Pane,
                            .at = DropPoint { .x = x, .y = y },
                            .split = SplitSupport::Supported,
                            .origin = DropOrigin::OtherTab };
}
} // namespace

TEST_CASE("dropZone.tabInEachEdgeBandSplitsThatSide", "[dropzone]")
{
    CHECK(resolveDropZone(tab(100, 250)) == DropZone::SplitLeft);
    CHECK(resolveDropZone(tab(950, 250)) == DropZone::SplitRight);
    CHECK(resolveDropZone(tab(500, 50)) == DropZone::SplitTop);
    CHECK(resolveDropZone(tab(500, 460)) == DropZone::SplitBottom);
}

TEST_CASE("dropZone.tabInTheMiddlePassesThrough", "[dropzone]")
{
    // Review Focus 2: a sloppy drop is not accepted, so the drag ends as today (tear-off).
    CHECK(resolveDropZone(tab(500, 250)) == DropZone::PassThrough);
}

TEST_CASE("dropZone.bandEdgeIsTheFractionOfTheExtent", "[dropzone]")
{
    constexpr auto Band = Pane.width * DropEdgeFraction;
    CHECK(resolveDropZone(tab(Band - 0.1, 250)) == DropZone::SplitLeft);
    CHECK(resolveDropZone(tab(Band, 250)) == DropZone::PassThrough);
    CHECK(resolveDropZone(tab(Pane.width - Band, 250)) == DropZone::PassThrough);
    CHECK(resolveDropZone(tab(Pane.width - Band + 0.1, 250)) == DropZone::SplitRight);
    constexpr auto BandV = Pane.height * DropEdgeFraction;
    CHECK(resolveDropZone(tab(500, BandV - 0.1)) == DropZone::SplitTop);
    CHECK(resolveDropZone(tab(500, BandV)) == DropZone::PassThrough);
}

TEST_CASE("dropZone.cornersPickTheNearerEdge", "[dropzone]")
{
    // Left 50/200 = 0.25 of its band, top 20/100 = 0.2 of its band.
    CHECK(resolveDropZone(tab(50, 20)) == DropZone::SplitTop);
    // Left 20/200 = 0.1, top 20/100 = 0.2.
    CHECK(resolveDropZone(tab(20, 20)) == DropZone::SplitLeft);
    // A tie (100/200 == 50/100) goes to the vertical edge, left or right.
    CHECK(resolveDropZone(tab(100, 50)) == DropZone::SplitLeft);
    CHECK(resolveDropZone(tab(900, 450)) == DropZone::SplitRight);
    CHECK(resolveDropZone(tab(900, 50)) == DropZone::SplitRight);
}

TEST_CASE("dropZone.filesAndTextNeverSplit", "[dropzone]")
{
    for (auto const payload: { DropPayload::Files, DropPayload::Text })
        for (auto const [x, y]:
             { std::pair { 50.0, 250.0 }, { 950.0, 250.0 }, { 500.0, 10.0 }, { 500.0, 250.0 } })
            CHECK(resolveDropZone({ .payload = payload,
                                    .pane = Pane,
                                    .at = { .x = x, .y = y },
                                    .split = SplitSupport::Supported,
                                    .origin = DropOrigin::OtherTab })
                  == DropZone::InsertText);
}

TEST_CASE("dropZone.ownTabAndUnsupportedWindowsPassThrough", "[dropzone]")
{
    // Review Focus 3, and a window whose sessions are hosted elsewhere.
    for (auto const [x, y]:
         { std::pair { 100.0, 250.0 }, { 950.0, 250.0 }, { 500.0, 50.0 }, { 500.0, 250.0 } })
    {
        auto own = tab(x, y);
        own.origin = DropOrigin::OwnTab;
        CHECK(resolveDropZone(own) == DropZone::PassThrough);
        auto unsupported = tab(x, y);
        unsupported.split = SplitSupport::Unsupported;
        CHECK(resolveDropZone(unsupported) == DropZone::PassThrough);
    }
}

TEST_CASE("dropZone.nothingToDropOn", "[dropzone]")
{
    CHECK(resolveDropZone(tab(-1, 250)) == DropZone::None);
    CHECK(resolveDropZone(tab(1000, 250)) == DropZone::None);
    CHECK(resolveDropZone(tab(500, 500)) == DropZone::None);
    auto empty = tab(0, 0);
    empty.pane = {};
    CHECK(resolveDropZone(empty) == DropZone::None);
    auto none = tab(100, 250);
    none.payload = DropPayload::None;
    CHECK(resolveDropZone(none) == DropZone::None);
}

TEST_CASE("dropZone.ghostIsTheInsetHalfOnTheZonesSide", "[dropzone]")
{
    CHECK(dropGhostOf(DropZone::SplitRight, Pane, 6) == DropRect { 506, 6, 488, 488 });
    CHECK(dropGhostOf(DropZone::SplitLeft, Pane, 6) == DropRect { 6, 6, 488, 488 });
    CHECK(dropGhostOf(DropZone::SplitTop, Pane, 6) == DropRect { 6, 6, 988, 238 });
    CHECK(dropGhostOf(DropZone::SplitBottom, Pane, 6) == DropRect { 6, 256, 988, 238 });
    for (auto const zone: { DropZone::None, DropZone::InsertText, DropZone::PassThrough })
        CHECK_FALSE(dropGhostOf(zone, Pane, 6).has_value());
}

TEST_CASE("dropZone.rowsFollowTheEnumeratorOrder", "[dropzone]")
{
    CHECK(DropZoneRows.size() == DropZoneCount);
    for (auto const [i, row]: std::views::enumerate(DropZoneRows))
        CHECK(static_cast<size_t>(row.zone) == static_cast<size_t>(i));
    CHECK(dropZoneRowOf(DropZone::SplitRight).label == "Tab → split right");
    CHECK(dropZoneRowOf(DropZone::SplitRight).side == SplitSide::Right);
    CHECK(dropZoneRowOf(DropZone::SplitTop).label == "Tab → split up");
    CHECK(dropZoneRowOf(DropZone::SplitBottom).label == "Tab → split down");
    CHECK(dropZoneRowOf(DropZone::SplitLeft).label == "Tab → split left");
    for (auto const zone: { DropZone::None, DropZone::InsertText, DropZone::PassThrough })
    {
        CHECK_FALSE(dropZoneRowOf(zone).side.has_value());
        CHECK(dropZoneRowOf(zone).label.empty());
    }
}
