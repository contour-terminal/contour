// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DropZonePolicy.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

using contour::window::DropEdgeFraction;
using contour::window::DropExtent;
using contour::window::dropGhostLabelOf;
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
using contour::window::MoveRefusal;
using contour::window::refusalTextOf;
using contour::window::RefusalTitle;
using contour::window::resolveDropZone;
using vtworkspace::SplitSide;

namespace
{
constexpr auto Pane = DropExtent { .width = 1000, .height = 500 };

constexpr DropZoneInputs tab(double x, double y)
{
    return DropZoneInputs { .payload = DropPayload::Tab,
                            .pane = Pane,
                            .at = DropPoint { .x = x, .y = y },
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
                                    .origin = DropOrigin::OtherTab })
                  == DropZone::InsertText);
}

TEST_CASE("dropZone.ownTabPassesThrough", "[dropzone]")
{
    // Review Focus 3.
    for (auto const [x, y]:
         { std::pair { 100.0, 250.0 }, { 950.0, 250.0 }, { 500.0, 50.0 }, { 500.0, 250.0 } })
    {
        auto own = tab(x, y);
        own.origin = DropOrigin::OwnTab;
        CHECK(resolveDropZone(own) == DropZone::PassThrough);
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
    for (auto const [i, row]: core::views::enumerate(DropZoneRows))
        CHECK(static_cast<size_t>(row.zone) == static_cast<size_t>(i));
    CHECK(dropZoneRowOf(DropZone::SplitRight).tabLabel == "Tab → split right");
    CHECK(dropZoneRowOf(DropZone::SplitRight).side == SplitSide::Right);
    CHECK(dropZoneRowOf(DropZone::SplitTop).tabLabel == "Tab → split up");
    CHECK(dropZoneRowOf(DropZone::SplitBottom).tabLabel == "Tab → split down");
    CHECK(dropZoneRowOf(DropZone::SplitLeft).tabLabel == "Tab → split left");
    for (auto const zone: { DropZone::None, DropZone::InsertText, DropZone::PassThrough })
    {
        CHECK_FALSE(dropZoneRowOf(zone).side.has_value());
        CHECK(dropZoneRowOf(zone).tabLabel.empty());
    }
}

namespace
{
constexpr DropZoneInputs pane(double x, double y)
{
    return DropZoneInputs { .payload = DropPayload::Pane,
                            .pane = Pane,
                            .at = DropPoint { .x = x, .y = y },
                            .origin = DropOrigin::OtherTab,
                            .refusal = MoveRefusal::None };
}
} // namespace

TEST_CASE("dropZone.paneInEachEdgeBandSplitsThatSide", "[dropzone]")
{
    CHECK(resolveDropZone(pane(100, 250)) == DropZone::SplitLeft);
    CHECK(resolveDropZone(pane(950, 250)) == DropZone::SplitRight);
    CHECK(resolveDropZone(pane(500, 50)) == DropZone::SplitTop);
    CHECK(resolveDropZone(pane(500, 460)) == DropZone::SplitBottom);
}

TEST_CASE("dropZone.paneInTheMiddleSwaps", "[dropzone]")
{
    CHECK(resolveDropZone(pane(500, 250)) == DropZone::Swap);
}

TEST_CASE("dropZone.paneOverItselfIsANoOp", "[dropzone]")
{
    for (auto const [x, y]:
         { std::pair { 100.0, 250.0 }, { 950.0, 250.0 }, { 500.0, 50.0 }, { 500.0, 250.0 } })
    {
        auto own = pane(x, y);
        own.origin = DropOrigin::OwnPane;
        CHECK(resolveDropZone(own) == DropZone::NoOp);
    }
    auto outside = pane(-1, 250);
    outside.origin = DropOrigin::OwnPane;
    CHECK(resolveDropZone(outside) == DropZone::None);
}

TEST_CASE("dropZone.paneFromItsOwnTabStillSplitsAndSwaps", "[dropzone]")
{
    auto own = pane(950, 250);
    own.origin = DropOrigin::OwnTab;
    CHECK(resolveDropZone(own) == DropZone::SplitRight);
    own.at = { .x = 500, .y = 250 };
    CHECK(resolveDropZone(own) == DropZone::Swap);
}

TEST_CASE("dropZone.multiPaneTabInTheMiddlePassesThrough", "[dropzone]")
{
    CHECK(resolveDropZone(tab(500, 250)) == DropZone::PassThrough);
    CHECK(resolveDropZone(tab(950, 250)) == DropZone::SplitRight);
}

TEST_CASE("dropZone.refusalWinsOverEveryZoneForMoves", "[dropzone]")
{
    for (auto const refusal: { MoveRefusal::OtherProcess, MoveRefusal::OldDaemon, MoveRefusal::Tmux })
        for (auto const payload: { DropPayload::Pane, DropPayload::Tab })
            for (auto const origin: { DropOrigin::OtherTab, DropOrigin::OwnTab, DropOrigin::OwnPane })
                for (auto const [x, y]:
                     { std::pair { 100.0, 250.0 }, { 950.0, 250.0 }, { 500.0, 50.0 }, { 500.0, 250.0 } })
                {
                    auto in = pane(x, y);
                    in.payload = payload;
                    in.origin = origin;
                    in.refusal = refusal;
                    CHECK(resolveDropZone(in) == DropZone::Refused);
                    in.at = { .x = -1, .y = 0 };
                    CHECK(resolveDropZone(in) == DropZone::None);
                }
}

TEST_CASE("dropZone.refusalNeverAffectsFilesOrText", "[dropzone]")
{
    for (auto const refusal: { MoveRefusal::OtherProcess, MoveRefusal::OldDaemon, MoveRefusal::Tmux })
        for (auto const payload: { DropPayload::Files, DropPayload::Text })
        {
            auto in = pane(500, 250);
            in.payload = payload;
            in.refusal = refusal;
            CHECK(resolveDropZone(in) == DropZone::InsertText);
        }
}

TEST_CASE("dropZone.labelsPerPayload", "[dropzone]")
{
    CHECK(dropGhostLabelOf(DropZone::SplitLeft, DropPayload::Pane) == "Pane → split left");
    CHECK(dropGhostLabelOf(DropZone::SplitRight, DropPayload::Pane) == "Pane → split right");
    CHECK(dropGhostLabelOf(DropZone::SplitTop, DropPayload::Pane) == "Pane → split up");
    CHECK(dropGhostLabelOf(DropZone::SplitBottom, DropPayload::Pane) == "Pane → split down");
    CHECK(dropGhostLabelOf(DropZone::Swap, DropPayload::Pane) == "Swap with %1");
    CHECK(dropGhostLabelOf(DropZone::SplitLeft, DropPayload::Tab) == "Tab → split left");
    CHECK(dropGhostLabelOf(DropZone::SplitRight, DropPayload::Tab) == "Tab → split right");
    CHECK(dropGhostLabelOf(DropZone::SplitTop, DropPayload::Tab) == "Tab → split up");
    CHECK(dropGhostLabelOf(DropZone::SplitBottom, DropPayload::Tab) == "Tab → split down");
    CHECK(dropGhostLabelOf(DropZone::Swap, DropPayload::Tab).empty());
    for (auto const zone:
         { DropZone::None, DropZone::InsertText, DropZone::PassThrough, DropZone::NoOp, DropZone::Refused })
        for (auto const payload: { DropPayload::Pane, DropPayload::Tab, DropPayload::Files })
            CHECK(dropGhostLabelOf(zone, payload).empty());
    for (auto const payload: { DropPayload::None, DropPayload::Files, DropPayload::Text })
        CHECK(dropGhostLabelOf(DropZone::SplitLeft, payload).empty());
}

TEST_CASE("dropZone.refusalTexts", "[dropzone]")
{
    CHECK(RefusalTitle == "Can't move a pane here");
    CHECK(refusalTextOf(MoveRefusal::None).empty());
    CHECK(refusalTextOf(MoveRefusal::OtherProcess)
          == "This window belongs to another Contour process. Run Contour in daemon mode to move panes "
             "between any windows.");
    CHECK(refusalTextOf(MoveRefusal::OldDaemon)
          == "This window's daemon is too old to move panes. Update Contour on the daemon side.");
    CHECK(refusalTextOf(MoveRefusal::Tmux) == "Panes can't be moved into a tmux window yet.");
}

TEST_CASE("dropZone.swapGhostIsTheWholePaneInset", "[dropzone]")
{
    CHECK(dropGhostOf(DropZone::Swap, Pane, 6) == DropRect { 6, 6, 988, 488 });
    CHECK_FALSE(dropGhostOf(DropZone::NoOp, Pane, 6).has_value());
    CHECK_FALSE(dropGhostOf(DropZone::Refused, Pane, 6).has_value());
}

TEST_CASE("dropZone.moveAnnouncements", "[dropzone]")
{
    using contour::window::moveAnnouncementOf;
    using contour::window::MoveLanding;
    CHECK(moveAnnouncementOf(DropPayload::Pane, MoveLanding::Beside) == "Pane moved");
    CHECK(moveAnnouncementOf(DropPayload::Pane, MoveLanding::BesideInOtherWindow)
          == "Pane moved to window %1");
    CHECK(moveAnnouncementOf(DropPayload::Pane, MoveLanding::Swapped) == "Panes swapped");
    CHECK(moveAnnouncementOf(DropPayload::Pane, MoveLanding::NewTab) == "Pane moved to new tab");
    CHECK(moveAnnouncementOf(DropPayload::Pane, MoveLanding::NewWindow) == "Pane moved to new window");
    for (auto const landing: { MoveLanding::Beside,
                               MoveLanding::BesideInOtherWindow,
                               MoveLanding::NewTab,
                               MoveLanding::NewWindow })
        CHECK(moveAnnouncementOf(DropPayload::Tab, landing) == "Tab moved");
    for (auto const payload: { DropPayload::None, DropPayload::Files, DropPayload::Text })
        CHECK(moveAnnouncementOf(payload, MoveLanding::Beside).empty());
}

TEST_CASE("dropZone.moveRouteTable", "[dropzone]")
{
    using contour::window::HostMoves;
    using contour::window::MovePlace;
    using contour::window::MoveRoute;
    using contour::window::moveRouteOf;
    using contour::window::MoveSubject;

    constexpr auto Places =
        std::array { MovePlace::Edge, MovePlace::EdgeOtherWindow, MovePlace::Swap, MovePlace::OnTab,
                     MovePlace::Gap,  MovePlace::NewWindow };
    constexpr auto Subjects = std::array { MoveSubject::Pane, MoveSubject::WholeTab };

    // A host that moves nodes takes every drop as one.
    for (auto const place: Places)
        for (auto const subject: Subjects)
            CHECK(moveRouteOf(HostMoves::Nodes, place, subject) == MoveRoute::Node);

    // The released daemon keeps what it served: a whole tab into a gap or out to a window of its own. A tmux
    // server serves nothing.
    struct Row
    {
        HostMoves host;
        MovePlace place;
        MoveSubject subject;
        MoveRoute route;
    };
    constexpr auto Served = std::array {
        Row { HostMoves::Tabs, MovePlace::Gap, MoveSubject::WholeTab, MoveRoute::TabMove },
        Row { HostMoves::Tabs, MovePlace::NewWindow, MoveSubject::WholeTab, MoveRoute::TabMove },
    };
    for (auto const host: { HostMoves::Tabs, HostMoves::Nothing })
        for (auto const place: Places)
            for (auto const subject: Subjects)
            {
                auto const row = std::ranges::find_if(Served, [&](Row const& served) {
                    return served.host == host && served.place == place && served.subject == subject;
                });
                auto const expected = row != Served.end() ? row->route : MoveRoute::Refused;
                INFO("host " << static_cast<int>(host) << " place " << static_cast<int>(place) << " subject "
                             << static_cast<int>(subject));
                CHECK(moveRouteOf(host, place, subject) == expected);
            }
}

TEST_CASE("dropZone.refusalTable", "[dropzone]")
{
    using contour::window::DragHost;
    using contour::window::HostMoves;
    using contour::window::moveRefusalOf;
    using contour::window::MoveRoute;

    struct Row
    {
        HostMoves host;
        DragHost from;
        MoveRoute route;
        MoveRefusal refusal;
    };
    constexpr auto Rows = std::array {
        Row { HostMoves::Nodes, DragHost::Own, MoveRoute::Node, MoveRefusal::None },
        Row { HostMoves::Nodes, DragHost::Other, MoveRoute::Node, MoveRefusal::OtherProcess },
        Row { HostMoves::Tabs, DragHost::Own, MoveRoute::TabMove, MoveRefusal::None },
        Row { HostMoves::Tabs, DragHost::Own, MoveRoute::Refused, MoveRefusal::OldDaemon },
        // Another host's drag is refused for that, before the old daemon is.
        Row { HostMoves::Tabs, DragHost::Other, MoveRoute::Refused, MoveRefusal::OtherProcess },
        // A tmux window refuses every drop, whoever's it is.
        Row { HostMoves::Nothing, DragHost::Own, MoveRoute::Refused, MoveRefusal::Tmux },
        Row { HostMoves::Nothing, DragHost::Other, MoveRoute::Refused, MoveRefusal::Tmux },
    };
    for (auto const& row: Rows)
    {
        INFO("host " << static_cast<int>(row.host) << " from " << static_cast<int>(row.from) << " route "
                     << static_cast<int>(row.route));
        CHECK(moveRefusalOf(row.host, row.from, row.route) == row.refusal);
    }
}
