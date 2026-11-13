// SPDX-License-Identifier: Apache-2.0
//
// The picker row's "open in new window" button per sidebar look: one row per look, and a face that means
// nothing else on its row (ui-ux rule 1.3).

#include <contour/window/PickerAccessory.hpp>
#include <contour/window/SidebarControls.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace contour::window;

TEST_CASE("PickerAccessoryRows lists every sidebar look, and each face draws something", "[switcher]")
{
    STATIC_REQUIRE(PickerAccessoryRows.size() == SidebarControlRows.size());
    for (auto const& row: PickerAccessoryRows)
    {
        INFO("look " << static_cast<int>(row.look));
        CHECK((!row.openInNewWindow.bundledIcon.empty() || !row.openInNewWindow.glyph.empty()));
        // A themed icon always has its bundled copy.
        CHECK((row.openInNewWindow.iconName.empty() || !row.openInNewWindow.bundledIcon.empty()));
    }
}

TEST_CASE("the open-in-new-window face means nothing else on its row", "[switcher]")
{
    for (auto const& row: SidebarControlRows)
    {
        INFO("look " << static_cast<int>(row.look));
        auto const& face = pickerAccessoryFace(row.look);
        for (auto const& control: row.faces)
        {
            CHECK(face
                  != PickerAccessoryFace { .iconName = control.iconName,
                                           .bundledIcon = control.bundledIcon,
                                           .glyph = control.glyph });
            if (!face.bundledIcon.empty())
                CHECK(face.bundledIcon != control.bundledIcon);
            if (!face.glyph.empty())
                CHECK(face.glyph != control.glyph);
        }
    }
}
