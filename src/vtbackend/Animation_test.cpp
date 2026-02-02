// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Animation.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using namespace std::chrono_literals;
using Catch::Approx;

// {{{ LinearEasing tests

TEST_CASE("LinearEasing.returns_identity", "[Animation]")
{
    auto constexpr Easing = LinearEasing {};
    CHECK(Easing(0.0f) == 0.0f);
    CHECK(Easing(0.25f) == 0.25f);
    CHECK(Easing(0.5f) == 0.5f);
    CHECK(Easing(0.75f) == 0.75f);
    CHECK(Easing(1.0f) == 1.0f);
}

// }}}

// {{{ EaseOutCubic tests

TEST_CASE("EaseOutCubic.boundary_values", "[Animation]")
{
    auto constexpr Easing = EaseOutCubic {};
    CHECK(Easing(0.0f) == Approx(0.0f));
    CHECK(Easing(1.0f) == Approx(1.0f));
}

TEST_CASE("EaseOutCubic.midpoint_above_linear", "[Animation]")
{
    auto constexpr Easing = EaseOutCubic {};
    // Ease-out cubic should always be above the linear diagonal for t in (0, 1).
    CHECK(Easing(0.25f) > 0.25f);
    CHECK(Easing(0.5f) > 0.5f);
    CHECK(Easing(0.75f) > 0.75f);
}

TEST_CASE("EaseOutCubic.known_midpoint_value", "[Animation]")
{
    auto constexpr Easing = EaseOutCubic {};
    // At t=0.5: 1 - (1 - 0.5)^3 = 1 - 0.125 = 0.875
    CHECK(Easing(0.5f) == Approx(0.875f));
}

// }}}

// {{{ AnimationState tests

namespace
{

/// Helper to create a time point offset from a base.
auto timeAt(std::chrono::steady_clock::time_point base, std::chrono::milliseconds offset)
    -> std::chrono::steady_clock::time_point
{
    return base + offset;
}

} // namespace

TEST_CASE("AnimationState.inactive_returns_complete", "[Animation]")
{
    auto const state = AnimationState<LinearEasing> {
        .active = false,
        .startTime = std::chrono::steady_clock::now(),
        .duration = 200ms,
    };
    auto const now = state.startTime + 50ms;
    CHECK(state.progress(now) == 1.0f);
    CHECK(state.isComplete(now));
}

TEST_CASE("AnimationState.zero_duration_returns_complete", "[Animation]")
{
    auto const state = AnimationState<LinearEasing> {
        .active = true,
        .startTime = std::chrono::steady_clock::now(),
        .duration = 0ms,
    };
    CHECK(state.progress(state.startTime) == 1.0f);
    CHECK(state.isComplete(state.startTime));
}

TEST_CASE("AnimationState.progress_at_start", "[Animation]")
{
    auto const base = std::chrono::steady_clock::now();
    auto const state = AnimationState<LinearEasing> {
        .active = true,
        .startTime = base,
        .duration = 1000ms,
    };
    CHECK(state.progress(base) == Approx(0.0f));
    CHECK_FALSE(state.isComplete(base));
}

TEST_CASE("AnimationState.progress_at_end", "[Animation]")
{
    auto const base = std::chrono::steady_clock::now();
    auto const state = AnimationState<LinearEasing> {
        .active = true,
        .startTime = base,
        .duration = 1000ms,
    };
    CHECK(state.progress(timeAt(base, 1000ms)) == Approx(1.0f));
    CHECK(state.isComplete(timeAt(base, 1000ms)));
}

TEST_CASE("AnimationState.progress_at_midpoint_linear", "[Animation]")
{
    auto const base = std::chrono::steady_clock::now();
    auto const state = AnimationState<LinearEasing> {
        .active = true,
        .startTime = base,
        .duration = 1000ms,
    };
    CHECK(state.progress(timeAt(base, 500ms)) == Approx(0.5f));
}

TEST_CASE("AnimationState.progress_at_midpoint_eased", "[Animation]")
{
    auto const base = std::chrono::steady_clock::now();
    auto const state = AnimationState<EaseOutCubic> {
        .active = true,
        .startTime = base,
        .duration = 1000ms,
    };
    // At t=0.5 with ease-out cubic: 1 - (1 - 0.5)^3 = 0.875
    CHECK(state.progress(timeAt(base, 500ms)) == Approx(0.875f));
}

TEST_CASE("AnimationState.progress_clamped_before_start", "[Animation]")
{
    auto const base = std::chrono::steady_clock::now();
    auto const state = AnimationState<LinearEasing> {
        .active = true,
        .startTime = base,
        .duration = 1000ms,
    };
    // Querying before the start time should clamp to 0.
    CHECK(state.progress(timeAt(base, -100ms)) == Approx(0.0f));
}

TEST_CASE("AnimationState.progress_clamped_after_end", "[Animation]")
{
    auto const base = std::chrono::steady_clock::now();
    auto const state = AnimationState<LinearEasing> {
        .active = true,
        .startTime = base,
        .duration = 1000ms,
    };
    // Querying well past the end should clamp to 1.
    CHECK(state.progress(timeAt(base, 5000ms)) == Approx(1.0f));
    CHECK(state.isComplete(timeAt(base, 5000ms)));
}

TEST_CASE("AnimationState.monotonically_increasing", "[Animation]")
{
    auto const base = std::chrono::steady_clock::now();
    auto const state = AnimationState<EaseOutCubic> {
        .active = true,
        .startTime = base,
        .duration = 1000ms,
    };
    auto prev = 0.0f;
    for (auto ms = 0; ms <= 1000; ms += 50)
    {
        auto const p = state.progress(timeAt(base, std::chrono::milliseconds(ms)));
        CHECK(p >= prev);
        prev = p;
    }
}

// }}}
