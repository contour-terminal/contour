// SPDX-License-Identifier: Apache-2.0
//
// Lifetime tests for the terminal's accessibility interfaces: nothing either of them registers with
// Qt's accessibility cache may outlive the display. ~TerminalAccessible explains why that is not
// automatic for the prompt, and what it cost when it was not enforced (issue #2015).
//
// Offscreen, so this gates in CI: an interface is created and cached independently of whether an
// assistive client is attached. The end-to-end counterpart -- a live session whose OSC 133 marks make
// the prompt genuinely appear -- is display-gated, in DisplayRendering_test.cpp.

#include <contour/display/TerminalAccessible.h>
#include <contour/display/TerminalDisplay.h>

#include <QtGui/QAccessible>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using contour::display::TerminalAccessible;
using contour::display::TerminalDisplay;

TEST_CASE("a11y: the terminal's interfaces leave Qt's accessibility cache with their display",
          "[contour][a11y]")
{
    TerminalAccessible::installFactory();

    auto display = std::make_unique<TerminalDisplay>();
    auto* accessible =
        dynamic_cast<TerminalAccessible*>(QAccessible::queryAccessibleInterface(display.get()));
    REQUIRE(accessible != nullptr);

    // uniqueId() registers on a miss, so both ids are live cache entries from here on. The terminal's
    // own interface is the control: it HAS a QObject, so Qt has always cleaned it up correctly.
    auto const terminalId = QAccessible::uniqueId(accessible);
    auto const promptId = QAccessible::uniqueId(accessible->promptInterface());
    REQUIRE(QAccessible::accessibleInterface(terminalId) == accessible);
    REQUIRE(QAccessible::accessibleInterface(promptId) == accessible->promptInterface());

    display.reset(); // QObject::destroyed -> the cache deletes the terminal's interface

    CHECK(QAccessible::accessibleInterface(terminalId) == nullptr);
    CHECK(QAccessible::accessibleInterface(promptId) == nullptr);
}
