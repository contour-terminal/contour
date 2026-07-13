// SPDX-License-Identifier: Apache-2.0
#include <contour/ContextMenuModel.h>

#include <QtCore/QString>
#include <QtCore/QVariantMap>

namespace contour
{

QVariantList toContextMenuModel(std::vector<ContextMenuEntry> const& entries,
                                std::vector<actions::Action>& actions)
{
    auto rows = QVariantList {};
    rows.reserve(static_cast<qsizetype>(entries.size()));

    for (auto const& entry: entries)
    {
        auto row = QVariantMap {};

        switch (entry.kind)
        {
            case ContextMenuEntryKind::Separator: //
                row["kind"] = QStringLiteral("separator");
                break;

            case ContextMenuEntryKind::Submenu:
                row["kind"] = QStringLiteral("submenu");
                row["title"] = QString::fromStdString(entry.title);
                // Recursing before the parent row is pushed keeps the ids in the order the rows are
                // walked, which is the order QML rebuilds the menu in.
                row["children"] = toContextMenuModel(entry.children, actions);
                break;

            case ContextMenuEntryKind::Command:
                row["kind"] = QStringLiteral("command");
                row["title"] = QString::fromStdString(entry.title);
                row["enabled"] = entry.enabled;
                row["checkable"] = entry.checkable;
                row["checked"] = entry.checked;
                row["actionId"] = static_cast<int>(actions.size());
                actions.push_back(entry.action);
                break;
        }

        rows.push_back(row);
    }

    return rows;
}

} // namespace contour
