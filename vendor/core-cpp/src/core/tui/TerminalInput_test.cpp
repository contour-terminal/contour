// SPDX-License-Identifier: Apache-2.0
//
// The mouse tracking mode TerminalInput asks for, without a terminal: nothing here enables the
// protocols, so nothing is written. posix/TerminalInput_test.cpp reads the bytes themselves.

#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/MouseTracking.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TerminalInput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using core::tui::MockTerminalOutput;
using core::tui::MouseTracking;
using core::tui::Terminal;
using core::tui::TerminalInput;

TEST_CASE("TerminalInput.mouse_tracking_defaults_to_off", "[tui]")
{
    auto const input = TerminalInput {};
    CHECK(input.mouseTracking() == MouseTracking::Off);
}

TEST_CASE("TerminalInput.mouse_tracking_is_the_requested_mode_raised_while_hover_is_on", "[tui]")
{
    auto input = TerminalInput {};
    input.setMouseTracking(MouseTracking::Drag);
    CHECK(input.mouseTracking() == MouseTracking::Drag);

    input.setAnyMotionTracking(true);
    CHECK(input.mouseTracking() == MouseTracking::AnyMotion);

    // The request made while raised is kept, and applies again once hover is off.
    input.setMouseTracking(MouseTracking::Buttons);
    CHECK(input.mouseTracking() == MouseTracking::AnyMotion);
    input.setAnyMotionTracking(false);
    CHECK(input.mouseTracking() == MouseTracking::Buttons);
}

TEST_CASE("Terminal.setMouseTracking_forwards_to_its_input", "[tui]")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    terminal.setMouseTracking(MouseTracking::Buttons);
    CHECK(terminal.input().mouseTracking() == MouseTracking::Buttons);
}
