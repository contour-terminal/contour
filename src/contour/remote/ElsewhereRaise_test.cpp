// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Notifier.hpp>
#include <contour/remote/ElsewhereRaise.hpp>
#include <contour/test/RecordingNotifier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace
{
using contour::test::RecordingNotifier;

/// @return What a pane that rang the bell and notified @p title raised.
[[nodiscard]] vthost::proto::PaneSummary rangAndNotified(std::string title)
{
    auto summary = vthost::proto::PaneSummary {};
    summary.session = 4;
    summary.bell = vthost::proto::PaneBell::Rang;
    summary.notifications.push_back({ .title = std::move(title),
                                      .body = "needs input",
                                      .occasion = vthost::proto::NotifyOccasion::Unstated });
    return summary;
}

/// @return The daemon `local:work` of run @p instance, named as an observed daemon's notices name it.
[[nodiscard]] contour::remote::RaisingDaemon observedWork(uint64_t instance = 1)
{
    return contour::remote::RaisingDaemon { .daemon = contour::window::localDaemonRef("work"),
                                            .instance = contour::window::DaemonInstance { instance },
                                            .name = "work" };
}

/// @return What a pane @p session raised as a long command in it finished.
[[nodiscard]] vthost::proto::PaneSummary finishedIn(uint64_t session)
{
    auto summary = vthost::proto::PaneSummary {};
    summary.session = session;
    summary.finished =
        vthost::proto::FinishedCommand { .exitCode = 0, .durationMs = 20'000, .commandLine = "make" };
    return summary;
}

} // namespace

TEST_CASE("ElsewhereRaise: a notice of an observed daemon names it", "[raise][presence]")
{
    auto notifier = RecordingNotifier {};
    auto const notices = contour::remote::ElsewhereNotices {
        .policy = {}, .bell = {}, .homeDirectory = "/home/test", .notifier = notifier
    };
    auto const raised = std::array { rangAndNotified("Build"), rangAndNotified({}) };

    auto const ring = contour::remote::raiseElsewhere(raised, {}, notices, observedWork());

    CHECK(ring == contour::remote::RingBell::Yes);
    REQUIRE(notifier.raised.size() == 2);
    for (auto const& notification: notifier.raised)
    {
        CHECK(notification.title.contains("work"));
        CHECK(notification.body == "needs input");
    }
    CHECK(notifier.raised.front().title.contains("Build"));
}

TEST_CASE("ElsewhereRaise: the window's own daemon's notices are worded as before", "[raise][link]")
{
    auto notifier = RecordingNotifier {};
    auto const notices = contour::remote::ElsewhereNotices {
        .policy = {}, .bell = {}, .homeDirectory = "/home/test", .notifier = notifier
    };
    auto const raised = std::array { rangAndNotified("Build"), rangAndNotified({}) };

    auto const ring = contour::remote::raiseElsewhere(
        raised,
        {},
        notices,
        { .daemon = contour::window::localDaemonRef("work"), .instance = {}, .name = {} });

    CHECK(ring == contour::remote::RingBell::Yes);
    REQUIRE(notifier.raised.size() == 2);
    CHECK(notifier.raised[0].title == "Build");
    CHECK(notifier.raised[1].title.empty());
}

TEST_CASE("ElsewhereRaise: nothing that rang asks for no bell", "[raise]")
{
    auto notifier = RecordingNotifier {};
    auto const notices = contour::remote::ElsewhereNotices {
        .policy = {}, .bell = {}, .homeDirectory = "/home/test", .notifier = notifier
    };
    auto summary = rangAndNotified("Build");
    summary.bell = vthost::proto::PaneBell::Silent;
    auto const raised = std::array { summary };

    CHECK(contour::remote::raiseElsewhere(raised, {}, notices, observedWork())
          == contour::remote::RingBell::No);
    CHECK(notifier.raised.size() == 1);
}

TEST_CASE("ElsewhereRaise: a finish replaces its own pane's notice, not another daemon's",
          "[raise][presence]")
{
    auto notifier = RecordingNotifier {};
    auto const notices = contour::remote::ElsewhereNotices {
        .policy = {}, .bell = {}, .homeDirectory = "/home/test", .notifier = notifier
    };
    auto const raised = std::array { finishedIn(4) };

    std::ignore = contour::remote::raiseElsewhere(raised, {}, notices, observedWork());
    std::ignore = contour::remote::raiseElsewhere(raised, {}, notices, observedWork());
    std::ignore = contour::remote::raiseElsewhere(
        raised,
        {},
        notices,
        { .daemon = contour::window::hostDaemonRef("starfive"), .instance = { 1 }, .name = "starfive" });
    std::ignore = contour::remote::raiseElsewhere(raised, {}, notices, observedWork(2));

    REQUIRE(notifier.raised.size() == 4);
    // The same pane of the same run: its later finish replaces the earlier one.
    CHECK(notifier.raised[0].identifier == notifier.raised[1].identifier);
    // Pane 4 of another daemon, or of the same daemon's next run, is another pane.
    CHECK(notifier.raised[2].identifier != notifier.raised[0].identifier);
    CHECK(notifier.raised[3].identifier != notifier.raised[0].identifier);
    CHECK(notifier.raised[3].identifier != notifier.raised[2].identifier);
}
