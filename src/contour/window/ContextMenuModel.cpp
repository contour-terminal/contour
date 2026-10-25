// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ColorConversion.hpp>
#include <contour/window/ContextMenuModel.hpp>

#include <QtCore/QString>
#include <QtCore/QVariantMap>

#include <utility>

namespace contour::window
{

namespace
{
    /// Whether @p node is drawn as a colour swatch rather than as a text row.
    [[nodiscard]] bool isSwatch(command::MenuNode const& node) noexcept
    {
        return node.kind == command::ContextMenuEntryKind::Command && node.swatch.has_value();
    }

    [[nodiscard]] bool isEnabled(command::MenuNode const& node) noexcept
    {
        return node.availability == command::RowAvailability::Enabled;
    }

    /// One entry of a `swatches` row.
    [[nodiscard]] QVariantMap swatchEntry(command::MenuNode const& node)
    {
        auto entry = QVariantMap {};
        entry["title"] = QString::fromStdString(node.title);
        entry["color"] = platform::toQColor(*node.swatch);
        entry["actionId"] = static_cast<int>(node.actionIndex);
        entry["enabled"] = isEnabled(node);
        entry["checked"] = node.check == command::RowCheck::Checked;
        return entry;
    }

    /// The row for one non-swatch node.
    [[nodiscard]] QVariantMap rowFor(command::MenuNode const& node)
    {
        auto row = QVariantMap {};
        switch (node.kind)
        {
            case command::ContextMenuEntryKind::Separator: //
                row["kind"] = QStringLiteral("separator");
                break;

            case command::ContextMenuEntryKind::Submenu:
                row["kind"] = QStringLiteral("submenu");
                row["title"] = QString::fromStdString(node.title);
                row["enabled"] = isEnabled(node);
                row["children"] = toContextMenuModel(node.children);
                break;

            case command::ContextMenuEntryKind::Command:
                row["kind"] = QStringLiteral("command");
                row["title"] = QString::fromStdString(node.title);
                row["enabled"] = isEnabled(node);
                row["checkable"] = node.check != command::RowCheck::None;
                row["checked"] = node.check == command::RowCheck::Checked;
                row["actionId"] = static_cast<int>(node.actionIndex);
                break;
        }
        return row;
    }
} // namespace

QVariantList toContextMenuModel(std::span<command::MenuNode const> nodes)
{
    auto rows = QVariantList {};
    rows.reserve(static_cast<qsizetype>(nodes.size()));

    // The swatches of the run being collected; flushed as one row when the run ends.
    auto swatches = QVariantList {};
    auto const flushSwatches = [&] {
        if (swatches.isEmpty())
            return;
        auto row = QVariantMap {};
        row["kind"] = QStringLiteral("swatches");
        row["swatches"] = std::exchange(swatches, QVariantList {});
        rows.push_back(row);
    };

    for (auto const& node: nodes)
    {
        if (isSwatch(node))
        {
            swatches.push_back(swatchEntry(node));
            continue;
        }
        flushSwatches();
        rows.push_back(rowFor(node));
    }
    flushSwatches();

    return rows;
}

} // namespace contour::window
