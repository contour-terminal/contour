// SPDX-License-Identifier: Apache-2.0
//
// When an application's notification is raised, for every occasion and every place its pane can be. Qt-free
// on purpose, like the visibility table it builds on, so contour_test covers it in both frontend
// configurations.

#include <contour/platform/NotificationOccasion.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>

using contour::platform::SessionVisibility;
using vtbackend::DisplayOccasion;

TEST_CASE("NotificationOccasion.an application's occasion is decided as a finish notice's when",
          "[notification]")
{
    constexpr auto Focused = SessionVisibility::Focused;
    constexpr auto Visible = SessionVisibility::VisibleUnfocused;
    constexpr auto Hidden = SessionVisibility::Hidden;

    struct Row
    {
        DisplayOccasion occasion = DisplayOccasion::Always;
        SessionVisibility visibility = SessionVisibility::Hidden;
        bool wanted = false;
    };

    // kitty's occasions (`notifications.py`, `is_notification_allowed`): `unfocused` declines while the pane
    // has keyboard focus; `invisible` also while it is on screen, unfocused or not.
    constexpr auto Table = std::array {
        Row { .occasion = DisplayOccasion::Always, .visibility = Focused, .wanted = true },
        Row { .occasion = DisplayOccasion::Always, .visibility = Visible, .wanted = true },
        Row { .occasion = DisplayOccasion::Always, .visibility = Hidden, .wanted = true },
        Row { .occasion = DisplayOccasion::Unfocused, .visibility = Focused, .wanted = false },
        Row { .occasion = DisplayOccasion::Unfocused, .visibility = Visible, .wanted = true },
        Row { .occasion = DisplayOccasion::Unfocused, .visibility = Hidden, .wanted = true },
        Row { .occasion = DisplayOccasion::Invisible, .visibility = Focused, .wanted = false },
        Row { .occasion = DisplayOccasion::Invisible, .visibility = Visible, .wanted = false },
        Row { .occasion = DisplayOccasion::Invisible, .visibility = Hidden, .wanted = true },
    };

    for (auto const& row: Table)
    {
        INFO(std::format(
            "occasion {}, visibility {}", static_cast<int>(row.occasion), static_cast<int>(row.visibility)));
        CHECK(contour::platform::isNoticeWanted(contour::platform::notifyWhenOf(row.occasion), row.visibility)
              == row.wanted);
    }
}

TEST_CASE("NotificationOccasion.OSC 777 and OSC 9 are raised unless the pane has focus", "[notification]")
{
    // foot's default (`inhibit-when-focused=yes`): neither sequence can state an occasion of its own.
    using contour::platform::isNoticeWanted;
    using contour::platform::notifyWhenOf;
    using contour::platform::UnstatedNotificationOccasion;

    STATIC_CHECK_FALSE(
        isNoticeWanted(notifyWhenOf(UnstatedNotificationOccasion), SessionVisibility::Focused));
    STATIC_CHECK(
        isNoticeWanted(notifyWhenOf(UnstatedNotificationOccasion), SessionVisibility::VisibleUnfocused));
    STATIC_CHECK(isNoticeWanted(notifyWhenOf(UnstatedNotificationOccasion), SessionVisibility::Hidden));
}
