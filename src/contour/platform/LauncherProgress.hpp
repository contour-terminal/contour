// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/TaskbarProgressRouter.hpp>

#include <QtCore/QString>
#include <QtCore/QVariantMap>

namespace contour::platform
{
/// Identifies the installed launcher, accepting Qt desktop names with or without the suffix.
/// @param desktopName QGuiApplication's desktop file name.
/// @return Unity LauncherEntry application URI, or an empty string when no identity is available.
[[nodiscard]] inline QString launcherProgressUri(QString desktopName)
{
    if (desktopName.isEmpty())
        return {};
    if (!desktopName.endsWith(QStringLiteral(".desktop")))
        desktopName += QStringLiteral(".desktop");
    return QStringLiteral("application://") + desktopName;
}

/// Maps OSC progress and the waiting-tab count to the Unity LauncherEntry protocol used by Plasma.
/// @param progress Application-wide aggregate. Busy uses a full bar because the protocol has no animation.
/// @param attention How many tabs wait for the user; shown as the launcher's count badge, hidden at zero.
/// @return Wire properties; error and paused retain their percentage without changing desktop urgency.
[[nodiscard]] inline QVariantMap launcherEntryProperties(TaskbarProgress progress, TaskbarAttention attention)
{
    return {
        { QStringLiteral("progress-visible"), progress.state != vtbackend::ProgressState::Inactive },
        { QStringLiteral("progress"),
          progress.state == vtbackend::ProgressState::Indeterminate ? 1.0 : progress.percentage / 100.0 },
        { QStringLiteral("count"), static_cast<qint64>(attention.waiting) },
        { QStringLiteral("count-visible"), attention.waiting > 0 },
    };
}
} // namespace contour::platform
