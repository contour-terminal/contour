// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A tab's icon (window::TabIcon) as QML reads it: the shape TabIconMark.qml takes, for the tab strip and the
/// sidebar's tab rows alike.

#include <contour/window/TabIcon.hpp>

#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVariantMap>

namespace contour::window
{

/// @param icon The icon.
/// @return { kind, text }: the kind as its underlying value, the text the image's file URL for an Image and
///         the initial otherwise.
[[nodiscard]] inline QVariantMap tabIconVariant(TabIcon const& icon)
{
    return { { QStringLiteral("kind"), static_cast<int>(icon.kind) },
             { QStringLiteral("text"),
               icon.kind == TabIconKind::Image
                   ? QUrl::fromLocalFile(QString::fromStdU16String(icon.image.u16string())).toString()
                   : QString::fromStdString(icon.initial) } };
}

} // namespace contour::window
