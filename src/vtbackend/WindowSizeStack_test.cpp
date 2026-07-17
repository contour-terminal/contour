// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/WindowSizeStack.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

namespace
{
constexpr auto Normal = PageSize { .lines = LineCount(24), .columns = ColumnCount(80) };
constexpr auto Screen = PageSize { .lines = LineCount(50), .columns = ColumnCount(200) };
} // namespace

TEST_CASE("WindowSizeStack.maximize_both", "[WindowSizeStack]")
{
    auto stack = WindowSizeStack {};

    auto const maximized = stack.maximize(WindowMaximize::Both, Normal, Screen);
    REQUIRE(maximized.has_value());
    CHECK(*maximized == Screen);
    CHECK(stack.enlarged());

    auto const restored = stack.maximize(WindowMaximize::Restore, Screen, Screen);
    REQUIRE(restored.has_value());
    CHECK(*restored == Normal);
    CHECK(!stack.enlarged());
}

TEST_CASE("WindowSizeStack.maximize_one_axis_leaves_the_other", "[WindowSizeStack]")
{
    SECTION("vertically")
    {
        auto stack = WindowSizeStack {};
        auto const size = stack.maximize(WindowMaximize::Vertically, Normal, Screen);
        REQUIRE(size.has_value());
        CHECK(size->lines == Screen.lines);
        CHECK(size->columns == Normal.columns);
    }

    SECTION("horizontally")
    {
        auto stack = WindowSizeStack {};
        auto const size = stack.maximize(WindowMaximize::Horizontally, Normal, Screen);
        REQUIRE(size.has_value());
        CHECK(size->lines == Normal.lines);
        CHECK(size->columns == Screen.columns);
    }
}

TEST_CASE("WindowSizeStack.restoring_a_normal_window_does_nothing", "[WindowSizeStack]")
{
    auto stack = WindowSizeStack {};
    CHECK(!stack.maximize(WindowMaximize::Restore, Normal, Screen).has_value());
}

TEST_CASE("WindowSizeStack.maximizing_twice_still_remembers_the_normal_size", "[WindowSizeStack]")
{
    // The size to come back to is the window's last *normal* size. Letting a second maximize overwrite
    // it would make the screen's size the one the window "came from", and it could never be restored.
    auto stack = WindowSizeStack {};

    CHECK(stack.maximize(WindowMaximize::Vertically, Normal, Screen).has_value());
    auto const again =
        stack.maximize(WindowMaximize::Both, PageSize { Screen.lines, Normal.columns }, Screen);
    REQUIRE(again.has_value());
    CHECK(*again == Screen);

    auto const restored = stack.maximize(WindowMaximize::Restore, Screen, Screen);
    REQUIRE(restored.has_value());
    CHECK(*restored == Normal);
}

TEST_CASE("WindowSizeStack.full_screen", "[WindowSizeStack]")
{
    auto stack = WindowSizeStack {};

    auto const entered = stack.fullScreen(WindowFullScreen::Enter, Normal, Screen);
    REQUIRE(entered.has_value());
    CHECK(*entered == Screen);

    auto const exited = stack.fullScreen(WindowFullScreen::Exit, Screen, Screen);
    REQUIRE(exited.has_value());
    CHECK(*exited == Normal);
}

TEST_CASE("WindowSizeStack.full_screen_toggle", "[WindowSizeStack]")
{
    auto stack = WindowSizeStack {};

    auto const entered = stack.fullScreen(WindowFullScreen::Toggle, Normal, Screen);
    REQUIRE(entered.has_value());
    CHECK(*entered == Screen);
    CHECK(stack.enlarged());

    auto const exited = stack.fullScreen(WindowFullScreen::Toggle, Screen, Screen);
    REQUIRE(exited.has_value());
    CHECK(*exited == Normal);
    CHECK(!stack.enlarged());
}

TEST_CASE("WindowSizeStack.exiting_a_normal_window_does_nothing", "[WindowSizeStack]")
{
    auto stack = WindowSizeStack {};
    CHECK(!stack.fullScreen(WindowFullScreen::Exit, Normal, Screen).has_value());
}
