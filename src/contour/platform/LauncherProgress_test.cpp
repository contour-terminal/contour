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
        auto const properties = launcherEntryProperties({ .state = state, .percentage = 60 }, {});
        CHECK(properties.value(QStringLiteral("progress-visible")).toBool());
        CHECK(properties.value(QStringLiteral("progress")).toDouble() == 0.6);
        CHECK(properties.size() == 4); // Progress and count only: no urgency side effects.
        CHECK_FALSE(properties.value(QStringLiteral("count-visible")).toBool());
    }
    CHECK(launcherEntryProperties({ .state = ProgressState::Normal }, {})
              .value(QStringLiteral("progress-visible"))
              .toBool());
    CHECK(launcherEntryProperties({ .state = ProgressState::Normal, .percentage = 100 }, {})
              .value(QStringLiteral("progress"))
              .toDouble()
          == 1.0);
    CHECK(launcherEntryProperties({ .state = ProgressState::Indeterminate }, {})
              .value(QStringLiteral("progress"))
              .toDouble()
          == 1.0);
    CHECK_FALSE(launcherEntryProperties({}, {}).value(QStringLiteral("progress-visible")).toBool());
}

TEST_CASE("Launcher entry carries the waiting-tab count beside the progress", "[contour][taskbar]")
{
    using namespace contour::platform;
    using vtbackend::ProgressState;

    auto const both =
        launcherEntryProperties({ .state = ProgressState::Normal, .percentage = 40 }, { .waiting = 2 });
    CHECK(both.value(QStringLiteral("progress-visible")).toBool());
    CHECK(both.value(QStringLiteral("progress")).toDouble() == 0.4);
    CHECK(both.value(QStringLiteral("count")).metaType() == QMetaType::fromType<qint64>());
    CHECK(both.value(QStringLiteral("count")).toLongLong() == 2);
    CHECK(both.value(QStringLiteral("count-visible")).toBool());

    auto const none = launcherEntryProperties({}, { .waiting = 0 });
    CHECK_FALSE(none.value(QStringLiteral("count-visible")).toBool());
    CHECK_FALSE(none.value(QStringLiteral("progress-visible")).toBool());

    // A count alone leaves the progress bar hidden, and the other way round.
    CHECK(launcherEntryProperties({}, { .waiting = 5 }).value(QStringLiteral("count-visible")).toBool());
    CHECK_FALSE(
        launcherEntryProperties({}, { .waiting = 5 }).value(QStringLiteral("progress-visible")).toBool());
}
