// SPDX-License-Identifier: Apache-2.0
//
// Where a pane's sticky command header lives (R-l): its own header, the title band, or over its top row.

#include <contour/window/StickyHeaderPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::window::PaneHeaderContent;
using contour::window::presentationOf;
using contour::window::StickyHeaderHost;
using contour::window::stickyHeaderHostFor;
using contour::window::StickyHeaderHostInputs;
using contour::window::TitleBandPresence;
using vtbackend::StickyHeaderPresentation;

TEST_CASE("stickyHeaderHostFor: a pane header takes it, else a lone pane's title band, else the grid",
          "[stickyheader]")
{
    // A header of its own, in a split with no band.
    CHECK(stickyHeaderHostFor(
              { .header = PaneHeaderContent::Name, .visiblePanes = 2, .band = TitleBandPresence::Absent })
          == StickyHeaderHost::PaneHeader);
    CHECK(
        stickyHeaderHostFor(
            { .header = PaneHeaderContent::NameAndCwd, .visiblePanes = 1, .band = TitleBandPresence::Shown })
        == StickyHeaderHost::PaneHeader);
    // The only pane, headerless, under a title band.
    CHECK(stickyHeaderHostFor(
              { .header = PaneHeaderContent::Hidden, .visiblePanes = 1, .band = TitleBandPresence::Shown })
          == StickyHeaderHost::TitleBand);
    // No title bar, or full screen with the band hidden.
    CHECK(stickyHeaderHostFor(
              { .header = PaneHeaderContent::Hidden, .visiblePanes = 1, .band = TitleBandPresence::Absent })
          == StickyHeaderHost::Grid);
    // Minimal splits: headerless panes share the band, so none of them owns it.
    CHECK(stickyHeaderHostFor(
              { .header = PaneHeaderContent::Hidden, .visiblePanes = 3, .band = TitleBandPresence::Shown })
          == StickyHeaderHost::Grid);
    // The defaults are today's: a lone headerless pane with no band keeps the in-grid band.
    CHECK(stickyHeaderHostFor(StickyHeaderHostInputs {}) == StickyHeaderHost::Grid);
}

TEST_CASE("presentationOf: only the grid draws the header over the cells", "[stickyheader]")
{
    CHECK(presentationOf(StickyHeaderHost::Grid) == StickyHeaderPresentation::InGrid);
    CHECK(presentationOf(StickyHeaderHost::PaneHeader) == StickyHeaderPresentation::Chrome);
    CHECK(presentationOf(StickyHeaderHost::TitleBand) == StickyHeaderPresentation::Chrome);
}
