// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the kitty pointer shape protocol (OSC 22).

#include <vtbackend/MockTerm.h>
#include <vtbackend/PointerShape.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace std::string_view_literals;
using namespace vtbackend;

TEST_CASE("PointerShape.supported_names", "[pointershape]")
{
    // Only shapes Contour can genuinely display. Claiming one it would not actually show is a lie an
    // application acts on.
    CHECK(pointer_shape::isSupportedName("default"));
    CHECK(pointer_shape::isSupportedName("text"));
    CHECK(pointer_shape::isSupportedName("pointer"));
    CHECK(pointer_shape::isSupportedName("none"));
    CHECK_FALSE(pointer_shape::isSupportedName("zoom-in"));
    CHECK_FALSE(pointer_shape::isSupportedName(""));
}

TEST_CASE("PointerShape.query_current", "[pointershape]")
{
    // Exactly the probe blessed sends: OSC 22 ; ?__current__ ST, answered with the CSS name.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033]22;?__current__\033\\"sv);
    CHECK(mock.terminal.peekInput() == std::format("\033]22;{}\033\\", pointer_shape::DefaultName));
}

TEST_CASE("PointerShape.query_reports_support_per_name", "[pointershape]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033]22;?text,zoom-in,pointer\033\\"sv);
    // 1 for shapes we can show, 0 for the one we cannot -- in the order asked.
    CHECK(mock.terminal.peekInput() == "\033]22;1,0,1\033\\");
}

TEST_CASE("PointerShape.set_replaces_the_current_shape", "[pointershape]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033]22;=pointer\033\\"sv);
    CHECK(mock.terminal.pointerShape() == "pointer");
}

TEST_CASE("PointerShape.the_leading_operation_char_is_optional_for_a_set", "[pointershape]")
{
    // "For set operations, the optional first char can be either `=` or omitted", and the spec's
    // very first worked example is `OSC 22 ; pointer ST`. Taking payload.front() as the operation
    // unconditionally turned the 'p' of "pointer" into an unknown operation, so the whole sequence
    // was rejected and the shape never changed.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033]22;pointer\033\\"sv);
    CHECK(mock.terminal.pointerShape() == "pointer");

    // The explicit form must keep working and mean the same thing.
    mock.writeToScreen("\033]22;=text\033\\"sv);
    CHECK(mock.terminal.pointerShape() == "text");
}

TEST_CASE("PointerShape.an_empty_payload_resets_to_the_default", "[pointershape]")
{
    // `OSC 22 ; ST` is the documented reset. Rejecting it as malformed left whatever shape the
    // application had set in place, with no way to put it back.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033]22;pointer\033\\"sv);
    REQUIRE(mock.terminal.pointerShape() == "pointer");

    mock.writeToScreen("\033]22;\033\\"sv);
    CHECK(mock.terminal.pointerShape() == pointer_shape::DefaultName);
}

TEST_CASE("PointerShape.returning_to_the_default_is_signalled_as_a_reset", "[pointershape]")
{
    // A frontend caches the application's shape so it survives the next mouse move -- but it needs to
    // know when the application has stopped imposing one, or its own screen-type defaults (the arrow
    // on the alternate screen) never apply again for the rest of the session. Popping back to the
    // bottom of the stack and resetting both mean "the terminal is free to use whatever it likes",
    // which is signalled by an empty name.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };

    SECTION("popping back to the bottom of the stack")
    {
        mock.writeToScreen("\033]22;>pointer\033\\"sv);
        REQUIRE(mock.pointerShapeNotifications.back() == "pointer");

        mock.writeToScreen("\033]22;<\033\\"sv);
        CHECK(mock.pointerShapeNotifications.back().empty());
    }

    SECTION("the documented reset form")
    {
        mock.writeToScreen("\033]22;pointer\033\\"sv);
        REQUIRE(mock.pointerShapeNotifications.back() == "pointer");

        mock.writeToScreen("\033]22;\033\\"sv);
        CHECK(mock.pointerShapeNotifications.back().empty());
    }

    SECTION("a hard reset")
    {
        mock.writeToScreen("\033]22;pointer\033\\"sv);
        REQUIRE(mock.pointerShapeNotifications.back() == "pointer");

        mock.writeToScreen("\033c"sv);
        CHECK(mock.pointerShapeNotifications.back().empty());
    }
}

TEST_CASE("PointerShape.push_and_pop_restore_the_previous_shape", "[pointershape]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033]22;=text\033\\"sv);
    mock.writeToScreen("\033]22;>pointer\033\\"sv);
    CHECK(mock.terminal.pointerShape() == "pointer");

    mock.writeToScreen("\033]22;<\033\\"sv);
    CHECK(mock.terminal.pointerShape() == "text");
}

TEST_CASE("PointerShape.popping_past_the_bottom_leaves_a_shape", "[pointershape]")
{
    // An application that pops more than it pushed must not be able to leave the terminal with no
    // pointer shape at all.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    for (int i = 0; i < 5; ++i)
        mock.writeToScreen("\033]22;<\033\\"sv);
    CHECK(mock.terminal.pointerShape() == pointer_shape::DefaultName);
}

TEST_CASE("PointerShape.unsupported_names_are_ignored_not_stored", "[pointershape]")
{
    // Storing a name we cannot display would make a later __current__ query answer with it.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033]22;=text\033\\"sv);
    mock.writeToScreen("\033]22;=zoom-in\033\\"sv);
    CHECK(mock.terminal.pointerShape() == "text");
}

TEST_CASE("PointerShape.a_push_list_leaves_the_last_name_current", "[pointershape]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033]22;>text,pointer\033\\"sv);
    CHECK(mock.terminal.pointerShape() == "pointer");

    // Both were pushed, so it takes two pops to get back.
    mock.writeToScreen("\033]22;<\033\\"sv);
    CHECK(mock.terminal.pointerShape() == "text");
}

TEST_CASE("PointerShape.push_depth_is_bounded", "[pointershape]")
{
    // An application looping on push must not grow the stack without limit. Past the cap the newest
    // shape still takes effect.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    for (int i = 0; i < 200; ++i)
        mock.writeToScreen("\033]22;>pointer\033\\"sv);
    CHECK(mock.terminal.pointerShape() == "pointer");

    // Still recoverable, and still never empty.
    for (int i = 0; i < 200; ++i)
        mock.writeToScreen("\033]22;<\033\\"sv);
    CHECK(mock.terminal.pointerShape() == pointer_shape::DefaultName);
}
