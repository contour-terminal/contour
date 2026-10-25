// SPDX-License-Identifier: Apache-2.0
#include <contour/window/ContextMenuModel.hpp>

#include <QtCore/QString>
#include <QtCore/QVariantMap>

namespace contour::window
{

QVariantList toContextMenuModel(std::vector<command::ContextMenuEntry> const& entries,
                                std::vector<actions::Action>& actions)
{
    auto rows = QVariantList {};
    rows.reserve(static_cast<qsizetype>(entries.size()));

    for (auto const& entry: entries)
    {
        auto row = QVariantMap {};

        switch (entry.kind)
        {
            case command::ContextMenuEntryKind::Separator: //
                row["kind"] = QStringLiteral("separator");
                break;

            case command::ContextMenuEntryKind::Submenu:
                row["kind"] = QStringLiteral("submenu");
                row["title"] = QString::fromStdString(entry.title);
                // Recursing before the parent row is pushed keeps the ids in the order the rows are
                // walked, which is the order QML rebuilds the menu in.
                row["children"] = toContextMenuModel(entry.children, actions);
                break;

            case command::ContextMenuEntryKind::Command:
                row["kind"] = QStringLiteral("command");
                row["title"] = QString::fromStdString(entry.title);
                row["enabled"] = entry.availability == command::RowAvailability::Enabled;
                row["checkable"] = entry.check != command::RowCheck::None;
                row["checked"] = entry.check == command::RowCheck::Checked;
                row["actionId"] = static_cast<int>(actions.size());
                actions.push_back(entry.action);
                break;
        }

        rows.push_back(row);
    }

    return rows;
}

} // namespace contour::window
