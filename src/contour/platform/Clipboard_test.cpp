// SPDX-License-Identifier: Apache-2.0
//
// Which of a copy's targets a desktop can take: the decision half of platform::copyToClipboard(), pinned
// for the desktop the test does not run on.

#include <contour/platform/Clipboard.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::platform::PrimarySelectionSupport;
using contour::platform::reachableTargets;
using vtbackend::ClipboardTarget;
using vtbackend::ClipboardTargets;

TEST_CASE("Clipboard.reachableTargets", "[contour][platform][clipboard]")
{
    constexpr auto ClipboardOnly = ClipboardTargets { ClipboardTarget::Clipboard };
    constexpr auto PrimaryOnly = ClipboardTargets { ClipboardTarget::PrimarySelection };
    constexpr auto Both = ClipboardTargets { ClipboardTarget::Clipboard, ClipboardTarget::PrimarySelection };

    SECTION("a desktop with a primary selection takes every target")
    {
        CHECK(reachableTargets(Both, PrimarySelectionSupport::Present) == Both);
        CHECK(reachableTargets(PrimaryOnly, PrimarySelectionSupport::Present) == PrimaryOnly);
        CHECK(reachableTargets(ClipboardOnly, PrimarySelectionSupport::Present) == ClipboardOnly);
    }

    SECTION("a desktop without one still takes the clipboard of a copy that names both")
    {
        CHECK(reachableTargets(Both, PrimarySelectionSupport::Absent) == ClipboardOnly);
        CHECK(reachableTargets(ClipboardOnly, PrimarySelectionSupport::Absent) == ClipboardOnly);
    }

    SECTION("a copy to the primary selection alone does not overwrite the clipboard instead")
    {
        CHECK(reachableTargets(PrimaryOnly, PrimarySelectionSupport::Absent).none());
    }
}
