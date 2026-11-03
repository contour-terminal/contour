// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ControlsStyle.hpp>

#include <QtCore/QFileInfo>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <algorithm>
#include <string>
#include <string_view>

namespace contour::platform
{

/// Whether a Qt Quick Controls style is installed beneath one of @p importPaths: its qmldir lies there, as
/// QQuickStyle resolves the name. Files are looked for, not imported: a style imported before
/// QQuickStyle::setStyle() would be loaded ahead of the one asked for. Qt's file API is used rather than the
/// standard library's because the application's own styles (ContourTui, ContourLinux) live in the
/// `:/qt/qml` resource tree. An import path given as a `qrc:` URL, as QQmlEngine lists the resource tree, is
/// looked up under the `:` prefix QFileInfo understands.
/// @param style       The style's name.
/// @param importPaths The import paths to search, normally QQmlEngine::importPathList() of the engine that
///                    will load the QML, so the answer is exactly what the engine will find.
/// @return Whether a qmldir of the style exists beneath one of them.
[[nodiscard]] inline bool isControlsStyleInstalled(std::string_view style, QStringList const& importPaths)
{
    return std::ranges::any_of(importPaths, [&](QString const& importPath) {
        auto const directory =
            importPath.startsWith(QStringLiteral("qrc:")) ? importPath.sliced(3) : importPath;
        return std::ranges::any_of(config::controlsStyleQmldirs(style), [&](std::string const& qmldir) {
            return QFileInfo::exists(directory + u'/' + QString::fromStdString(qmldir));
        });
    });
}

} // namespace contour::platform
