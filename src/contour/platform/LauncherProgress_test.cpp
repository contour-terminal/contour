// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/LauncherProgress.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Launcher progress names the desktop file and maps percentage-only states", "[contour][taskbar]")
{
    using namespace contour::platform;
    using vtbackend::ProgressState;
    CHECK(launcherProgressUri(QStringLiteral("contour")) == QStringLiteral("application://contour.desktop"));
    CHECK(launcherProgressUri(QStringLiteral("org.contourterminal.Contour.desktop"))
          == QStringLiteral("application://org.contourterminal.Contour.desktop"));
    CHECK(launcherProgressUri({}).isEmpty());

    for (auto const state: { ProgressState::Normal, ProgressState::Error, ProgressState::Paused })
    {
        auto const properties = launcherProgressProperties({ .state = state, .percentage = 60 });
        CHECK(properties.value(QStringLiteral("progress-visible")).toBool());
        CHECK(properties.value(QStringLiteral("progress")).toDouble() == 0.6);
        CHECK(properties.size() == 2); // No urgency/badge side effects.
    }
    CHECK(launcherProgressProperties({ .state = ProgressState::Normal })
              .value(QStringLiteral("progress-visible"))
              .toBool());
    CHECK(launcherProgressProperties({ .state = ProgressState::Normal, .percentage = 100 })
              .value(QStringLiteral("progress"))
              .toDouble()
          == 1.0);
    CHECK(launcherProgressProperties({ .state = ProgressState::Indeterminate })
              .value(QStringLiteral("progress"))
              .toDouble()
          == 1.0);
    CHECK_FALSE(launcherProgressProperties({}).value(QStringLiteral("progress-visible")).toBool());
}
