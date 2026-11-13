// SPDX-License-Identifier: Apache-2.0
//
// The daemon and workspace switcher popup, offscreen. Its controller's behaviour is SwitcherController_test's;
// this file holds what needs a window.

#include <contour/window/PopupParent.hpp>

#include <QtGui/QWindow>

#include <catch2/catch_test_macros.hpp>

using namespace contour;

TEST_CASE("Switcher: a popup reopened while it has the focus stays over its window", "[contour][switcher]")
{
    auto terminal = QWindow {};
    auto popup = QWindow {};
    CHECK(window::popupParentFor(&terminal, popup) == &terminal);
    CHECK(window::popupParentFor(nullptr, popup) == nullptr);

    popup.setTransientParent(&terminal);
    // Focused itself, the popup is not made its own parent: it keeps the one it has.
    CHECK(window::popupParentFor(&popup, popup) == &terminal);
}
