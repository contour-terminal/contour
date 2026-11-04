// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The drift chip following a daemon connection through a live upgrade.

#include <contour/remote/NativeController.hpp>
#include <contour/window/DaemonDriftModel.hpp>

#include <QtCore/QMetaObject>
#include <QtCore/QObject>

#include <functional>
#include <utility>

namespace contour::remote
{

/// Has @p drift follow @p controller's connection through a live upgrade
/// (DaemonDriftModel::followConnection): "Upgrading daemon…" while the connection is held for the daemon's
/// return, whichever client asked. Wired for every controller, local or remote: a daemon another client
/// upgrades holds every window's connection, and one this client cannot upgrade still says why its windows
/// wait.
///
/// The state is read where it changed, on the controller's reactor, and handed to @p drift on its own thread:
/// read there later, a short hold would already be over and never show.
/// @param controller The connection followed. Its signal is delivered while it runs; it must be stopped
/// before
///        @p drift goes.
/// @param drift The chip's model. Not owned.
/// @param connected Runs on @p drift's thread whenever the connection is up again, told whether the daemon
///        came back from an upgrade (DriftReassessment::Reassess), so that the drift is assessed anew; a
///        caller also reassesses a daemon that came back on another release.
/// @return The connection, for a caller that ends the following before the controller goes.
inline QMetaObject::Connection followDaemonDrift(NativeController& controller,
                                                 window::DaemonDriftModel& drift,
                                                 std::function<void(window::DriftReassessment)> connected)
{
    return QObject::connect(
        &controller,
        &NativeController::connectionStateChanged,
        &drift,
        [&controller, &drift, connected = std::move(connected)] {
            QMetaObject::invokeMethod(
                &drift,
                [&drift, state = controller.connectionState(), connected] {
                    auto const reassessment = drift.followConnection(state);
                    if (state == vthost::client::ConnectionState::Connected)
                        connected(reassessment);
                },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection);
}

} // namespace contour::remote
