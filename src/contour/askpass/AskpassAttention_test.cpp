// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassAttention.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace contour::askpass;
using State = SinkExposure::State;

TEST_CASE("a prompt is attended when no sink exists, so that it gets a window of its own",
          "[askpass][attention]")
{
    auto const attention = AskpassAttention {};
    CHECK(attention.isAttended(AnyWindow));
    CHECK(attention.isAttended(3));
}

TEST_CASE("the window that would show the prompt decides, not any window", "[askpass][attention]")
{
    auto attention = AskpassAttention {};
    attention.publish({ { .route = 1, .state = State::Hidden }, { .route = 2, .state = State::OnScreen } });

    // Its own window is hidden: held, though another is on screen.
    CHECK_FALSE(attention.isAttended(1));
    CHECK(attention.isAttended(2));
    // No window of its own, or a gone one: the first window shows it, and that one is hidden.
    CHECK_FALSE(attention.isAttended(AnyWindow));
    CHECK_FALSE(attention.isAttended(99));

    attention.publish({ { .route = 1, .state = State::OnScreen }, { .route = 2, .state = State::Hidden } });
    CHECK(attention.isAttended(1));
    CHECK_FALSE(attention.isAttended(2));
    CHECK(attention.isAttended(AnyWindow));
}

TEST_CASE("the stand-in window is where a prompt goes only when no real window exists",
          "[askpass][attention]")
{
    auto attention = AskpassAttention {};
    attention.publish({ { .route = BootstrapRoute, .state = State::OnScreen } });
    CHECK(attention.isAttended(AnyWindow));

    attention.publish(
        { { .route = BootstrapRoute, .state = State::OnScreen }, { .route = 5, .state = State::Hidden } });
    CHECK_FALSE(attention.isAttended(AnyWindow));
}
