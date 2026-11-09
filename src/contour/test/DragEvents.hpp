// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QPointF>
#include <QtCore/QtGlobal>

namespace contour::test
{

/// Qt 6.12 gave the drag events QPointF constructors and deprecated the QPoint ones.
/// @param scene The drag's position in scene coordinates.
/// @return @p scene in the type the installed Qt's drag-event constructors take.
[[nodiscard]] inline auto dragPosition(QPointF scene)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    return scene;
#else
    return scene.toPoint();
#endif
}

} // namespace contour::test
