// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/DesktopNotification.hpp>

#include <QtCore/QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <utility>

namespace contour::test
{

/// A notification with nothing interesting about it but its identifier, which is what the routing
/// cases are all actually about.
/// @param identifier The OSC 99 notification identifier.
/// @return The notification.
[[nodiscard]] inline vtbackend::DesktopNotification aNotification(std::string identifier)
{
    auto notification = vtbackend::DesktopNotification {};
    notification.identifier = std::move(identifier);
    notification.title = "Title";
    notification.body = "Body";
    return notification;
}

/// Whether this run may put a notification on a real bus.
///
/// Gated so a run on a developer's own desktop does not pop a notification up at them and leave it
/// there. The wedged-bus harness sets it, and there by construction nothing can be displayed -- so
/// the send path is covered exactly where covering it is harmless.
/// @return Whether to send.
[[nodiscard]] inline bool notificationSendingEnabled()
{
    return qEnvironmentVariableIsSet("CONTOUR_TEST_NOTIFICATION_SEND");
}

/// Runs @p exercise against whatever session bus the machine has, and requires it to have returned
/// promptly.
///
/// Every other case in these files talks to a recorder, which by construction cannot catch a
/// backend that WAITS -- and waiting is the regression the whole transport seam exists to prevent.
/// So the timed cases build their backend exactly as production does, and
/// test/e2e/notification-nonblocking.sh is what makes the bus interesting: there the notification
/// service is activatable and never answers, which is what a desktop with no working notification
/// backend looks like, and what used to cost 25 seconds per session. @see issue #2051.
///
/// The bound is deliberately generous: it separates "returned promptly" from "waited out a
/// 25-second D-Bus reply timeout", and is not trying to measure anything finer than that.
///
/// @p exercise runs only where sending is permitted, so the gate lives here rather than as a
/// prologue every future timed case has to remember -- forgetting it is what pops a notification up
/// at whoever ran the suite. The timing bracket is taken either way: returning promptly with
/// nothing to send is still the property under test.
///
/// @param exercise What to drive against the real bus.
template <typename Exercise>
void checkReturnsWithoutWaiting(Exercise exercise)
{
    auto const startedAt = std::chrono::steady_clock::now();

    if (notificationSendingEnabled())
        exercise();
    QCoreApplication::processEvents();

    CHECK(std::chrono::steady_clock::now() - startedAt < std::chrono::seconds { 5 });
}

} // namespace contour::test
