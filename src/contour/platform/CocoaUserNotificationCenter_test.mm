// SPDX-License-Identifier: Apache-2.0
//
// The macOS factory's gate: the test binary is no `.app`, so it must hand back the null centre and never
// touch `UNUserNotificationCenter`, whose `currentNotificationCenter` raises outside an application bundle.

#include <contour/platform/UserNotificationCenter.hpp>

#include <QtGui/QGuiApplication>

#include <catch2/catch_test_macros.hpp>

using contour::platform::AppBundle;
using contour::platform::currentAppBundle;
using contour::platform::makeUserNotificationCenter;
using contour::platform::UserNotificationAvailability;

TEST_CASE("CocoaUserNotificationCenter.aBareBinaryGetsTheNullCentre", "[cocoa][usernotifications]")
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        SKIP("Needs the cocoa platform plugin");

    CHECK(currentAppBundle() == AppBundle::Absent);
    CHECK(makeUserNotificationCenter()->availability() == UserNotificationAvailability::Unavailable);
}
