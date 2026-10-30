// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>

#include <catch2/catch_test_macros.hpp>

#include <QtTest/QTest>

namespace contour::test
{

/// How long a test waits for a popup's open or close transition at most, in milliseconds.
inline constexpr int PopupSettleMs = 2000;

/// Waits until @p popup has finished opening (its enter transition is over).
/// @param popup A Qt Quick Controls Popup.
inline void waitForPopupOpened(QObject const& popup)
{
    REQUIRE(QTest::qWaitFor([&] { return popup.property("opened").toBool(); }, PopupSettleMs));
}

/// Waits until @p popup has finished closing (its exit transition is over), then lets what it deferred to
/// the next event-loop turn run (a palette's picked command, for one).
/// @param popup A Qt Quick Controls Popup.
inline void waitForPopupClosed(QObject const& popup)
{
    REQUIRE(QTest::qWaitFor([&] { return !popup.property("visible").toBool(); }, PopupSettleMs));
    QCoreApplication::processEvents();
}

} // namespace contour::test
