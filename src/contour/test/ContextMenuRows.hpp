// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>

namespace contour::test
{

/// Finds a row of a context menu as a window publishes it to QML (window::toContextMenuModel()).
/// @param rows The menu's rows.
/// @param title The row's title.
/// @return The first row titled @p title, searching submenus too; an empty map for none.
[[nodiscard]] inline QVariantMap menuRowOf(QVariantList const& rows, QString const& title)
{
    for (auto const& value: rows)
    {
        auto row = value.toMap();
        if (row["title"].toString() == title)
            return row;
        if (auto nested = menuRowOf(row["children"].toList(), title); !nested.isEmpty())
            return nested;
    }
    return {};
}

} // namespace contour::test
