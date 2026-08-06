// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the taskbar's progress policy: how several sessions' OSC 9;4 reports reduce to the
// one thing a window's taskbar button can show, and when a report goes stale. Both decisions are
// driven here with a stated clock rather than a real one, which is the point of keeping them out of
// the platform backend.

#include <contour/platform/TaskbarProgressRouter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;
using contour::platform::TaskbarProgress;
using contour::platform::TaskbarProgressRouter;
using vtbackend::Progress;
using vtbackend::ProgressState;

namespace
{
/// A fixed origin, so every case states its times relative to something explicit.
constexpr auto Epoch = std::chrono::steady_clock::time_point {};

constexpr Progress at(ProgressState state, uint8_t percentage)
{
    return Progress { .state = state, .percentage = percentage };
}
} // namespace

TEST_CASE("TaskbarProgressRouter.nothing_reported_shows_nothing", "[contour][taskbar][progress]")
{
    auto const router = TaskbarProgressRouter {};
    CHECK(router.resolve(Epoch) == TaskbarProgress {});
}

TEST_CASE("TaskbarProgressRouter.a_single_session_is_shown_as_is", "[contour][taskbar][progress]")
{
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Normal, 40), Epoch);
    CHECK(router.resolve(Epoch) == TaskbarProgress { .state = ProgressState::Normal, .percentage = 40 });
}

TEST_CASE("TaskbarProgressRouter.inactive_withdraws_rather_than_reporting_zero",
          "[contour][taskbar][progress]")
{
    // A remembered zero would keep the taskbar showing a bar with nothing behind it.
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Normal, 40), Epoch);
    router.onProgress(1, at(ProgressState::Inactive, 0), Epoch);
    CHECK(router.resolve(Epoch) == TaskbarProgress {});
}

TEST_CASE("TaskbarProgressRouter.a_failure_outranks_a_sibling_still_running", "[contour][taskbar][progress]")
{
    // The ordering that matters: the protocol numbers Indeterminate (3) above Error (2), but a
    // failed build must not be hidden by a busy one in the only indicator the window has.
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Normal, 10), Epoch);
    router.onProgress(2, at(ProgressState::Indeterminate, 0), Epoch);
    router.onProgress(3, at(ProgressState::Error, 55), Epoch);
    CHECK(router.resolve(Epoch).state == ProgressState::Error);
}

TEST_CASE("TaskbarProgressRouter.paused_outranks_running_but_not_error", "[contour][taskbar][progress]")
{
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Normal, 10), Epoch);
    router.onProgress(2, at(ProgressState::Paused, 20), Epoch);
    CHECK(router.resolve(Epoch).state == ProgressState::Paused);

    router.onProgress(3, at(ProgressState::Error, 30), Epoch);
    CHECK(router.resolve(Epoch).state == ProgressState::Error);
}

TEST_CASE("TaskbarProgressRouter.among_equals_the_least_complete_wins", "[contour][taskbar][progress]")
{
    // The bar should say how much work is left, not how nearly done the furthest-along part is.
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Normal, 90), Epoch);
    router.onProgress(2, at(ProgressState::Normal, 25), Epoch);
    router.onProgress(3, at(ProgressState::Normal, 60), Epoch);
    CHECK(router.resolve(Epoch) == TaskbarProgress { .state = ProgressState::Normal, .percentage = 25 });
}

TEST_CASE("TaskbarProgressRouter.a_closed_session_stops_counting", "[contour][taskbar][progress]")
{
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Error, 50), Epoch);
    router.onProgress(2, at(ProgressState::Normal, 70), Epoch);
    router.onSessionClosed(1);
    CHECK(router.resolve(Epoch) == TaskbarProgress { .state = ProgressState::Normal, .percentage = 70 });
}

TEST_CASE("TaskbarProgressRouter.with_no_timeout_progress_never_goes_stale", "[contour][taskbar][progress]")
{
    // The default, and the spec-literal behaviour: progress persists until the application clears it.
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Normal, 40), Epoch);
    CHECK(router.resolve(Epoch + 24h).state == ProgressState::Normal);
}

TEST_CASE("TaskbarProgressRouter.a_configured_timeout_expires_a_stalled_report",
          "[contour][taskbar][progress]")
{
    // The opt-in guard against an application that dies mid-operation and strands a bar.
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Normal, 40), Epoch);

    CHECK(router.resolve(Epoch + 14s, 15s).state == ProgressState::Normal); // still fresh
    CHECK(router.resolve(Epoch + 15s, 15s) == TaskbarProgress {});          // exactly at the timeout
    CHECK(router.resolve(Epoch + 1h, 15s) == TaskbarProgress {});
}

TEST_CASE("TaskbarProgressRouter.an_update_refreshes_the_deadline", "[contour][taskbar][progress]")
{
    // What an application updating once a second relies on: each report restarts the clock.
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Normal, 40), Epoch);
    router.onProgress(1, at(ProgressState::Normal, 50), Epoch + 10s);
    CHECK(router.resolve(Epoch + 20s, 15s)
          == TaskbarProgress { .state = ProgressState::Normal, .percentage = 50 });
    CHECK(router.resolve(Epoch + 26s, 15s) == TaskbarProgress {});
}

TEST_CASE("TaskbarProgressRouter.a_stale_session_does_not_mask_a_live_one", "[contour][taskbar][progress]")
{
    // The case the timeout exists for: a dead application's Error must not outrank the live build.
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Error, 30), Epoch);
    router.onProgress(2, at(ProgressState::Normal, 70), Epoch + 20s);
    CHECK(router.resolve(Epoch + 20s, 15s)
          == TaskbarProgress { .state = ProgressState::Normal, .percentage = 70 });
}

TEST_CASE("TaskbarProgressRouter.the_timeout_is_a_per_call_policy", "[contour][taskbar][progress]")
{
    // The router remembers reports, not policy, so the caller can hand it the CURRENT configured
    // value on every query -- which is how a config reload takes effect with no second wiring step,
    // and why nothing here can drift out of step with the config.
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Normal, 40), Epoch);

    CHECK(router.resolve(Epoch + 1h).state == ProgressState::Normal);      // expiry off (default)
    CHECK(router.resolve(Epoch + 1h, 0ms).state == ProgressState::Normal); // explicitly off
    CHECK(router.resolve(Epoch + 1h, 15s) == TaskbarProgress {});          // and on again
    CHECK(router.resolve(Epoch + 5s, 15s).state == ProgressState::Normal); // same report, still fresh
}
