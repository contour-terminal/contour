// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QString>
#include <QtGui/QGuiApplication>

namespace contour::platform
{

/// @return Whether the running platform plugin is the native Windows one, the only one with an HWND. Asked on
///         every call rather than at construction, which precedes the application (F1).
[[nodiscard]] inline bool onNativeWindowsPlatform()
{
    return QGuiApplication::platformName() == QStringLiteral("windows");
}

} // namespace contour::platform
