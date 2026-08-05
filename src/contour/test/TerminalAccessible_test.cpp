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

namespace
{

/// Counts the events Qt is handed, and how many carry the child index that means "discard me".
///
/// Static state in a free function because QAccessible::installUpdateHandler takes a plain function
/// pointer, so there is nowhere to put a capture.
struct EventProbe
{
    static inline int Emitted = 0;
    static inline int StrayChild = 0;

    static void reset() noexcept
    {
        Emitted = 0;
        StrayChild = 0;
    }

    static void handle(QAccessibleEvent* event)
    {
        ++Emitted;
        // An event that names a QObject must leave the union's other member alone: -1 is "no child",
        // and anything else sends Qt looking for a child that does not exist. An event with no QObject
        // is the interface-subject form, where the union legitimately holds a unique id.
        if (event->object() != nullptr && event->child() != -1)
            ++StrayChild;
    }
};

} // namespace

TEST_CASE("a11y: the terminal's interfaces leave Qt's accessibility cache with their display",
          "[contour][a11y]")
{
    contour::display::TerminalAccessible::installFactory();

    auto display = std::make_unique<contour::display::TerminalDisplay>();
    auto* accessible = dynamic_cast<contour::display::TerminalAccessible*>(
        QAccessible::queryAccessibleInterface(display.get()));
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

TEST_CASE("a11y: every emitted event names a subject Qt can resolve", "[contour][a11y]")
{
    // QAccessibleEvent keeps m_child and m_uniqueId in a UNION, and its QAccessibleInterface overload
    // writes the id into that union while ALSO setting m_object. So for an interface that HAS a QObject
    // -- which TerminalAccessible does -- Qt takes its QObject branch and reads the id back as a child
    // index. Ids are allocated from 0x80000000 up, so the lookup gets a large negative number, fails,
    // and Qt DISCARDS the event: the caret report simply never reaches the OS, with nothing but
    // "Invalid child in QAccessibleEvent" on stderr to say so. See notifySubject() in
    // TerminalAccessible.cpp, which picks the overload by whether there is a QObject to name.
    contour::display::TerminalAccessible::installFactory();

    auto display = std::make_unique<contour::display::TerminalDisplay>();
    auto* accessible = dynamic_cast<contour::display::TerminalAccessible*>(
        QAccessible::queryAccessibleInterface(display.get()));
    REQUIRE(accessible != nullptr);

    EventProbe::reset();
    auto* const previous = QAccessible::installUpdateHandler(&EventProbe::handle);
    accessible->reportLocation();
    QAccessible::installUpdateHandler(previous);

    CHECK(EventProbe::Emitted > 0); // the probe really saw the emission
    CHECK(EventProbe::StrayChild == 0);
}
