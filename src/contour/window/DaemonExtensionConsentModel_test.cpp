// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DaemonExtensionConsentModel.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>

using contour::window::DaemonConsentAllowHold;
using contour::window::DaemonExtensionConsentModel;
using namespace std::chrono_literals;

namespace
{

/// One answer the model sent.
struct Sent
{
    std::string name;
    std::string grantDigest;
    vthost::GrantState answer;

    bool operator==(Sent const&) const = default;
};

/// @return The orchestrator's question for @p grantDigest.
[[nodiscard]] vthost::proto::ExtensionPermissionRequest question(std::string grantDigest = "9a1b")
{
    return vthost::proto::ExtensionPermissionRequest {
        .name = "orchestrator",
        .grantDigest = std::move(grantDigest),
        .title = "“orchestrator” wants to run in the background",
        .lines = { vthost::proto::WireConsentLine { .text = "Module: orchestrator@/x.wasm", .weight = 0 },
                   vthost::proto::WireConsentLine { .text = "gh-auth (short, in repos): gh auth status",
                                                    .weight = 1 } },
    };
}

/// A model on a manual clock, its hold timers kept for the case to fire, its answers recorded.
struct Fixture
{
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} };
    std::vector<std::function<void()>> timers;
    std::vector<Sent> sent;
    DaemonExtensionConsentModel model {
        [this](std::string name, std::string grantDigest, vthost::GrantState answer) {
            sent.push_back(
                Sent { .name = std::move(name), .grantDigest = std::move(grantDigest), .answer = answer });
        },
        clock,
        [this](QObject* /*context*/, std::chrono::milliseconds delay, std::function<void()> job) {
            CHECK(delay == DaemonConsentAllowHold);
            timers.push_back(std::move(job));
        }
    };

    /// Lets the hold pass, as the GUI's timer would.
    void passHold()
    {
        clock.advance(DaemonConsentAllowHold);
        for (auto const& timer: std::exchange(timers, {}))
            timer();
    }
};

} // namespace

TEST_CASE("DaemonExtensionConsentModel: show puts the question up", "[contour][consent]")
{
    auto f = Fixture {};
    CHECK_FALSE(f.model.isVisible());
    auto focus = QSignalSpy { &f.model, &DaemonExtensionConsentModel::focusRequested };
    f.model.show(question());
    CHECK(f.model.isVisible());
    CHECK(f.model.title() == QStringLiteral("“orchestrator” wants to run in the background"));
    CHECK(f.model.lines()
          == QStringList { QStringLiteral("Module: orchestrator@/x.wasm"),
                           QStringLiteral("gh-auth (short, in repos): gh auth status") });
    CHECK(focus.count() == 1);
}

TEST_CASE("DaemonExtensionConsentModel: Allow is held after the question is shown", "[contour][consent]")
{
    auto f = Fixture {};
    auto const window = f.model.attachViewer();
    f.model.setViewerActive(window, true);
    f.model.show(question());
    CHECK(f.model.isAllowHeldFor(window));
    f.model.allowFrom(window);
    CHECK(f.sent.empty());
    CHECK(f.model.isVisible());

    f.clock.advance(DaemonConsentAllowHold - 1ms);
    CHECK(f.model.isAllowHeldFor(window));
    auto changed = QSignalSpy { &f.model, &DaemonExtensionConsentModel::changed };
    f.passHold();
    CHECK_FALSE(f.model.isAllowHeldFor(window));
    CHECK(changed.count() >= 1); // the dialog's Allow comes alive

    SECTION("a re-show of the same question restarts nothing and asks for no focus")
    {
        auto focus = QSignalSpy { &f.model, &DaemonExtensionConsentModel::focusRequested };
        f.model.show(question());
        CHECK_FALSE(f.model.isAllowHeldFor(window));
        CHECK(focus.count() == 0);
        CHECK(f.timers.empty());
    }
    SECTION("a different question restarts the hold")
    {
        auto focus = QSignalSpy { &f.model, &DaemonExtensionConsentModel::focusRequested };
        f.model.show(question("ffff"));
        CHECK(f.model.isAllowHeldFor(window));
        CHECK(focus.count() == 1);
        f.model.allowFrom(window);
        CHECK(f.sent.empty());
    }
    SECTION("once the hold passed, allow grants and closes")
    {
        f.model.allowFrom(window);
        CHECK(f.sent
              == std::vector { Sent {
                  .name = "orchestrator", .grantDigest = "9a1b", .answer = vthost::GrantState::Granted } });
        CHECK_FALSE(f.model.isVisible());
    }
}

TEST_CASE("DaemonExtensionConsentModel: the hold is each window's, and restarts as it becomes active",
          "[contour][consent]")
{
    auto f = Fixture {};
    auto const front = f.model.attachViewer();
    auto const behind = f.model.attachViewer();
    f.model.setViewerActive(front, true);
    f.model.show(question());
    // The hold elapses while the other window is not active: Allow is not armed there.
    f.passHold();
    CHECK_FALSE(f.model.isAllowHeldFor(front));
    CHECK(f.model.isAllowHeldFor(behind));
    f.model.allowFrom(behind);
    CHECK(f.sent.empty());

    // The click that activates it must not land on an armed Allow: activation holds it anew.
    f.model.setViewerActive(front, false);
    f.model.setViewerActive(behind, true);
    CHECK(f.model.isAllowHeldFor(front));
    CHECK(f.model.isAllowHeldFor(behind));
    f.model.allowFrom(behind);
    CHECK(f.sent.empty());
    f.clock.advance(DaemonConsentAllowHold - 1ms);
    f.model.allowFrom(behind);
    CHECK(f.sent.empty());
    f.passHold();
    f.model.allowFrom(behind);
    CHECK(f.sent
          == std::vector { Sent {
              .name = "orchestrator", .grantDigest = "9a1b", .answer = vthost::GrantState::Granted } });

    // A window gone is no viewer; an unknown one is always held.
    f.model.detachViewer(front);
    f.model.show(question("ffff"));
    f.passHold();
    CHECK(f.model.isAllowHeldFor(front));
}

TEST_CASE("DaemonExtensionConsentModel: clear drops every question of a lost connection",
          "[contour][consent]")
{
    auto f = Fixture {};
    f.model.show(question());
    f.model.show(question("ffff"));
    f.model.clear();
    CHECK_FALSE(f.model.isVisible());
    CHECK(f.sent.empty());
    // Asked again by the next connection, it is shown again.
    f.model.show(question());
    CHECK(f.model.isVisible());
}

TEST_CASE("DaemonExtensionConsentModel: deny and dismiss", "[contour][consent]")
{
    auto f = Fixture {};
    f.model.show(question());
    SECTION("deny refuses at once, hold or not, and closes")
    {
        f.model.deny();
        CHECK(f.sent
              == std::vector { Sent {
                  .name = "orchestrator", .grantDigest = "9a1b", .answer = vthost::GrantState::Denied } });
        CHECK_FALSE(f.model.isVisible());
    }
    SECTION("dismiss closes and answers nothing")
    {
        f.model.dismiss();
        CHECK(f.sent.empty());
        CHECK_FALSE(f.model.isVisible());
        // Sent again, it is shown again.
        f.model.show(question());
        CHECK(f.model.isVisible());
    }
}

TEST_CASE("DaemonExtensionConsentModel: settle closes only the question it names", "[contour][consent]")
{
    auto f = Fixture {};
    f.model.show(question());
    SECTION("the shown one")
    {
        f.model.settle("orchestrator", "9a1b");
        CHECK_FALSE(f.model.isVisible());
        CHECK(f.sent.empty());
    }
    SECTION("another digest")
    {
        f.model.settle("orchestrator", "ffff");
        CHECK(f.model.isVisible());
        CHECK(f.sent.empty());
    }
    SECTION("the one beneath a newer question")
    {
        f.model.show(question("ffff"));
        f.model.settle("orchestrator", "9a1b");
        CHECK(f.model.isVisible());
        auto const window = f.model.attachViewer();
        f.model.setViewerActive(window, true);
        f.passHold();
        f.model.allowFrom(window);
        CHECK(f.sent
              == std::vector { Sent {
                  .name = "orchestrator", .grantDigest = "ffff", .answer = vthost::GrantState::Granted } });
        CHECK_FALSE(f.model.isVisible());
    }
}
