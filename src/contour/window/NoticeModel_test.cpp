// SPDX-License-Identifier: Apache-2.0
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/window/NoticeModel.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

#include <QtTest/QSignalSpy>

using namespace std::chrono_literals;
using contour::window::Notice;
using contour::window::NoticeAction;
using contour::window::NoticeKind;
using contour::window::NoticeModel;

namespace
{

/// A model over a clock the test moves, an announcer it reads, and a record of the actions taken.
struct ModelUnderTest
{
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} + 1h };
    contour::test::RecordingAnnouncer announcer;
    std::optional<NoticeAction> action;
    std::string subject; ///< What the last action taken acted on.
    NoticeModel model { clock, announcer, [this](NoticeAction taken, std::string const& on) {
                           action = taken;
                           subject = on;
                       } };
};

} // namespace

TEST_CASE("NoticeModel: a toast shows, is said once, and stays while hovered", "[contour][notices]")
{
    auto t = ModelUnderTest {};
    auto changes = QSignalSpy { &t.model, &NoticeModel::toastChanged };

    t.model.post(Notice { .kind = NoticeKind::Copied, .text = "Copied 3 lines" });
    CHECK(t.model.toastShown());
    CHECK(t.model.toastText() == "Copied 3 lines");
    CHECK(t.model.toastGlyph().isEmpty());
    CHECK(changes.count() == 1);
    REQUIRE(t.announcer.said.size() == 1);
    CHECK(t.announcer.said.front().message == "Copied 3 lines");
    CHECK(t.announcer.said.front().politeness == QAccessible::AnnouncementPoliteness::Polite);

    t.model.setToastHovered(true);
    t.clock.advance(10s);
    t.model.expireDue();
    CHECK(t.model.toastShown());

    t.model.setToastHovered(false);
    t.clock.advance(2s);
    t.model.expireDue();
    CHECK_FALSE(t.model.toastShown());
    CHECK(t.model.toastText().isEmpty());
    CHECK(changes.count() == 2);
    CHECK(t.announcer.said.size() == 1);
}

TEST_CASE("NoticeModel: a banner offers its action and can be dismissed", "[contour][notices]")
{
    auto t = ModelUnderTest {};
    auto changes = QSignalSpy { &t.model, &NoticeModel::bannerChanged };

    t.model.post(Notice { .kind = NoticeKind::ConfigReloadFailed, .text = "Contour could not reload" });
    CHECK(t.model.bannerText() == "Contour could not reload");
    CHECK(t.model.bannerGlyph() == "!");
    CHECK(t.model.bannerActionText() == "Show in Editor");
    CHECK_FALSE(t.model.toastShown());
    CHECK(changes.count() == 1);
    CHECK(t.announcer.said.size() == 1);

    t.model.triggerBannerAction();
    CHECK(t.action == NoticeAction::OpenConfigFile);

    t.model.dismissBanner();
    CHECK(t.model.bannerText().isEmpty());
    CHECK(t.model.bannerActionText().isEmpty());
    CHECK(changes.count() == 2);

    // Nothing to act on, nothing taken.
    t.action.reset();
    t.model.triggerBannerAction();
    CHECK_FALSE(t.action.has_value());
}

TEST_CASE("NoticeModel: a toast with an action runs it with its subject, then ends",
          "[contour][notices][notice]")
{
    auto t = ModelUnderTest {};
    t.model.post(contour::window::daemonHiddenNotice("starfive", "host:starfive"));
    REQUIRE(t.model.toastShown());
    CHECK(t.model.toastText() == "starfive hidden from the sidebar");
    CHECK(t.model.toastActionText() == "Undo");

    t.model.triggerToastAction();

    CHECK(t.action == NoticeAction::UndoHide);
    CHECK(t.subject == "host:starfive");
    CHECK_FALSE(t.model.toastShown());
    CHECK(t.model.toastActionText().isEmpty());
}

TEST_CASE("NoticeModel: a toast without an action has no action text", "[contour][notices][notice]")
{
    auto t = ModelUnderTest {};
    t.model.post(Notice { .kind = NoticeKind::Copied, .text = "Copied 3 lines" });
    CHECK(t.model.toastActionText().isEmpty());
    t.model.triggerToastAction();
    CHECK_FALSE(t.action.has_value());
    // Nothing to take: the toast stays its time.
    CHECK(t.model.toastShown());
}
