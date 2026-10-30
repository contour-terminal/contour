// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the taskbar's progress policy: how several sessions' OSC 9;4 reports reduce to the
// one thing a window's taskbar button can show, and when a report goes stale. Both decisions are
// driven here with a stated clock rather than a real one, which is the point of keeping them out of
// the platform backend.

#include <contour/platform/TaskbarProgressRouter.hpp>
#include <contour/platform/TaskbarProgressState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <format>
#include <utility>
#include <vector>

#ifdef _WIN32
    #include <contour/platform/TaskbarProgressSink.hpp>

    #include <core/log/LogSink.hpp>

    #include <QtGui/QGuiApplication>
    #include <QtGui/QWindow>

    #include <QtTest/QTest>
#endif

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

TEST_CASE("Taskbar progress retains updates until ready and replays after recreation",
          "[contour][taskbar][progress]")
{
    auto button = contour::platform::TaskbarProgressState {};
    auto const normal = TaskbarProgress { .state = ProgressState::Normal, .percentage = 40 };
    button.set(normal);
    CHECK_FALSE(button.pending());
    button.buttonCreated();
    CHECK(button.pending() == normal);
    CHECK(button.pending() == normal); // A failed native call must not acknowledge the update.
    button.applied();
    CHECK_FALSE(button.pending());
    button.buttonCreated();
    CHECK(button.pending() == normal); // Explorer recreated the button.
    button.applied();
    button.surfaceDestroyed();
    button.set({});
    CHECK_FALSE(button.pending());
    button.buttonCreated();
    CHECK(button.pending() == TaskbarProgress {});
}

TEST_CASE("The taskbar overlay follows the same readiness rule as progress", "[contour][taskbar][progress]")
{
    auto overlay = contour::platform::TaskbarAttentionState {};
    overlay.set({ .waiting = 2 });
    CHECK_FALSE(overlay.pending()); // No button yet: retained, not sent.
    overlay.buttonCreated();
    CHECK(overlay.pending() == contour::platform::TaskbarAttention { .waiting = 2 });
    overlay.applied();
    CHECK_FALSE(overlay.pending());
    overlay.set({});
    CHECK(overlay.pending() == contour::platform::TaskbarAttention {}); // Clearing is an update too.
    overlay.surfaceDestroyed();
    CHECK_FALSE(overlay.pending());
}

TEST_CASE("An invalidated taskbar overlay is sent again, and only on a ready button",
          "[contour][taskbar][progress]")
{
    auto overlay = contour::platform::TaskbarAttentionState {};
    overlay.set({ .waiting = 2 });
    overlay.invalidate();
    CHECK_FALSE(overlay.pending()); // No button yet: invalidating does not make one.
    overlay.buttonCreated();
    overlay.applied();
    REQUIRE_FALSE(overlay.pending());
    overlay.invalidate();
    CHECK(overlay.pending() == contour::platform::TaskbarAttention { .waiting = 2 });
}

TEST_CASE("Taskbar progress window selection preserves original report deadlines",
          "[contour][taskbar][progress]")
{
    auto router = TaskbarProgressRouter {};
    router.onProgress(1, at(ProgressState::Normal, 40), Epoch);
    router.onProgress(2, at(ProgressState::Error, 10), Epoch + 1s);
    auto const first = [](uint64_t session) {
        return session == 1;
    };
    CHECK(router.resolveFor(first, Epoch + 1s, 2s).state == ProgressState::Normal);
    CHECK(router.resolve(Epoch + 1s, 2s).state == ProgressState::Error);
    CHECK(router.nextExpiry(Epoch + 1s, 2s) == Epoch + 2s);
    CHECK(router.resolveFor(first, Epoch + 2s, 2s).state == ProgressState::Inactive);
    CHECK(router.nextExpiry(Epoch + 2s, 2s) == Epoch + 3s);
    CHECK_FALSE(router.nextExpiry(Epoch + 3s, 2s));
    CHECK_FALSE(router.nextExpiry(Epoch + 1s, 0ms));
}

TEST_CASE("Windows taskbar updates order values and states and stop on failure",
          "[contour][taskbar][progress]")
{
    using contour::platform::applyWindowsTaskbarProgress;
    auto states = std::vector<ProgressState> {};
    auto values = std::vector<uint8_t> {};
    auto const setState = [&](ProgressState state) -> std::expected<void, int> {
        states.push_back(state);
        return {};
    };
    auto const setValue = [&](uint8_t value) -> std::expected<void, int> {
        CHECK(states.empty()); // Determinate value must precede the final state.
        values.push_back(value);
        return {};
    };

    SECTION("normal, error and paused are determinate")
    {
        for (auto const state: { ProgressState::Normal, ProgressState::Error, ProgressState::Paused })
        {
            states.clear();
            values.clear();
            REQUIRE(applyWindowsTaskbarProgress({ .state = state, .percentage = 40 }, setValue, setState));
            CHECK(values == std::vector<uint8_t> { 40 });
            CHECK(states == std::vector { state });
        }
    }
    SECTION("busy clears the old bar and never writes a value")
    {
        REQUIRE(applyWindowsTaskbarProgress({ .state = ProgressState::Indeterminate }, setValue, setState));
        CHECK(values.empty());
        CHECK(states == std::vector { ProgressState::Inactive, ProgressState::Indeterminate });
    }
    SECTION("clearing only writes the state")
    {
        REQUIRE(applyWindowsTaskbarProgress({}, setValue, setState));
        CHECK(values.empty());
        CHECK(states == std::vector { ProgressState::Inactive });
    }
    SECTION("a failed first call prevents the second call")
    {
        auto const failed = [](auto) -> std::expected<void, int> {
            return std::unexpected(42);
        };
        auto const normal = applyWindowsTaskbarProgress({ .state = ProgressState::Normal }, failed, setState);
        REQUIRE_FALSE(normal);
        CHECK(normal.error() == 42);
        CHECK(states.empty());
        auto const busy =
            applyWindowsTaskbarProgress({ .state = ProgressState::Indeterminate }, setValue, failed);
        REQUIRE_FALSE(busy);
        CHECK(busy.error() == 42);
        CHECK(values.empty());
    }
}

#ifdef _WIN32
TEST_CASE("Windows taskbar publishes to a real button and replays after surface recreation",
          "[contour][taskbar-native][display]")
{
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        SKIP("Requires CONTOUR_TEST_DISPLAY=1 and the Windows Qt platform.");
    auto capture = core::log::ScopedCapture { "taskbar.progress" };
    auto sink = contour::platform::makeTaskbarProgressSink();
    QWindow window;
    window.setTitle(QStringLiteral("Contour taskbar progress smoke test"));
    window.resize(480, 120);
    sink->bindWindow(1, window);
    sink->setProgress(1, { .state = ProgressState::Normal, .percentage = 60 });
    window.show();
    REQUIRE(QTest::qWaitFor([&] { return capture.text().contains("state 1, progress 60%"); }, 5000));

    window.setWindowState(Qt::WindowMinimized);
    for (auto const state: { ProgressState::Error, ProgressState::Indeterminate, ProgressState::Paused })
    {
        auto const before = capture.text().size();
        sink->setProgress(1, { .state = state, .percentage = 60 });
        auto const log = capture.text().substr(before);
        CHECK(log.contains(std::format("state {}, progress 60%", std::to_underlying(state))));
        CHECK_FALSE(log.contains("failed"));
    }
    window.destroy();
    auto const before = capture.text().size();
    sink->setProgress(1, { .state = ProgressState::Normal, .percentage = 75 });
    CHECK(capture.text().size() == before); // No calls against the destroyed HWND.
    window.setWindowState(Qt::WindowNoState);
    window.show();
    REQUIRE(QTest::qWaitFor([&] { return capture.text().substr(before).contains("state 1, progress 75%"); },
                            5000));
    sink->removeWindow(1);
    CHECK(capture.text().contains("state 0, progress 0%"));
    CHECK_FALSE(capture.text().contains("failed"));
}
#endif

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

TEST_CASE("TaskbarAttention.the_dock_badge_text_is_the_count_and_nothing_for_none", "[contour][taskbar]")
{
    CHECK(contour::platform::dockBadgeText({}).empty());
    CHECK(contour::platform::dockBadgeText({ .waiting = 1 }) == "1");
    CHECK(contour::platform::dockBadgeText({ .waiting = 12 }) == "12");
}
