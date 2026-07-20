// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the caret-report gate. The terminal's cursorPositionChanged() fires on the cursor BLINK
// (the render buffer has no cursor while it is blinked off), so a stationary cursor would announce itself
// twice a second. The gate is fed the blink-free state and must collapse that to silence, while still
// reporting the transitions an assistive client genuinely needs.

#include <contour/display/CaretReportGate.h>

#include <catch2/catch_test_macros.hpp>

using contour::display::CaretReportGate;
using contour::display::CaretState;
using vtbackend::ColumnOffset;
using vtbackend::LineOffset;

namespace
{

CaretState visibleAt(int line, int column)
{
    return { .visible = true,
             .position = { .line = LineOffset(line), .column = ColumnOffset(column) },
             .prompt = std::nullopt };
}

CaretState hidden()
{
    return { .visible = false, .position = {}, .prompt = std::nullopt };
}

vtbackend::LivePromptSpan promptSpan(int firstLine, int lastLine, int inputBegin)
{
    return { .firstLine = LineOffset(firstLine),
             .lastLine = LineOffset(lastLine),
             .inputBegin = ColumnOffset(inputBegin) };
}

} // namespace

TEST_CASE("CaretReportGate.a stationary blinking cursor is reported exactly once", "[contour][a11y]")
{
    // THE case this class exists for. The blink makes the terminal fire repeatedly at the same
    // blink-free state; everything after the first must be swallowed.
    auto gate = CaretReportGate {};

    CHECK(gate.shouldReport(visibleAt(3, 5)));
    for (auto i = 0; i < 10; ++i)
        CHECK_FALSE(gate.shouldReport(visibleAt(3, 5)));
}

TEST_CASE("CaretReportGate.a move is reported", "[contour][a11y]")
{
    auto gate = CaretReportGate {};

    CHECK(gate.shouldReport(visibleAt(3, 5)));
    CHECK(gate.shouldReport(visibleAt(3, 6)));
    CHECK(gate.shouldReport(visibleAt(4, 6)));
}

TEST_CASE("CaretReportGate.becoming visible again is reported even at the same position", "[contour][a11y]")
{
    // Required behaviour: the client stopped tracking when the caret went away, so it must be told when
    // the caret comes back -- position unchanged or not.
    auto gate = CaretReportGate {};

    CHECK(gate.shouldReport(visibleAt(3, 5)));
    CHECK_FALSE(gate.shouldReport(hidden()));
    CHECK(gate.shouldReport(visibleAt(3, 5)));
}

TEST_CASE("CaretReportGate.becoming invisible is never reported", "[contour][a11y]")
{
    // There is no caret to point at, so there is nothing to say. The state is still recorded, which is
    // what makes the transition back reportable.
    auto gate = CaretReportGate {};

    CHECK(gate.shouldReport(visibleAt(3, 5)));
    CHECK_FALSE(gate.shouldReport(hidden()));
    CHECK_FALSE(gate.shouldReport(hidden()));
}

TEST_CASE("CaretReportGate.entering a prompt is reported without the caret moving", "[contour][a11y]")
{
    // The shell repaints its prompt around a caret that never moved; the region the client should be
    // pointing at changed all the same.
    auto gate = CaretReportGate {};

    auto outside = visibleAt(3, 5);
    CHECK(gate.shouldReport(outside));

    auto inside = outside;
    inside.prompt = promptSpan(3, 3, 2);
    CHECK(gate.shouldReport(inside));

    // ... and holding still inside it goes quiet again.
    CHECK_FALSE(gate.shouldReport(inside));
}

TEST_CASE("CaretReportGate.a prompt that moves is reported", "[contour][a11y]")
{
    auto gate = CaretReportGate {};

    auto state = visibleAt(3, 5);
    state.prompt = promptSpan(3, 3, 2);
    CHECK(gate.shouldReport(state));

    // The viewport scrolled: same caret cell, different prompt lines.
    state.prompt = promptSpan(2, 2, 2);
    CHECK(gate.shouldReport(state));
}

TEST_CASE("CaretReportGate.leaving a prompt is reported", "[contour][a11y]")
{
    auto gate = CaretReportGate {};

    auto state = visibleAt(3, 5);
    state.prompt = promptSpan(3, 3, 2);
    CHECK(gate.shouldReport(state));

    state.prompt = std::nullopt; // a command started
    CHECK(gate.shouldReport(state));
}

TEST_CASE("CaretReportGate.reset forces the next visible state to be reported", "[contour][a11y]")
{
    // Focus moved to another pane and back. The client is now pointing somewhere else, so this pane must
    // re-announce itself even though nothing about its own caret changed.
    auto gate = CaretReportGate {};

    CHECK(gate.shouldReport(visibleAt(3, 5)));
    CHECK_FALSE(gate.shouldReport(visibleAt(3, 5)));

    gate.reset();

    CHECK(gate.shouldReport(visibleAt(3, 5)));
}

TEST_CASE("CaretReportGate.an invisible caret is never reported first", "[contour][a11y]")
{
    auto gate = CaretReportGate {};

    CHECK_FALSE(gate.shouldReport(hidden()));
}
