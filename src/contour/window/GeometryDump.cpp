// SPDX-License-Identifier: Apache-2.0
#include <contour/window/GeometryDump.hpp>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QSaveFile>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <utility>

namespace contour::window
{

namespace
{
    /// A property value as JSON, or undefined where it is none of the kinds a dump reports.
    [[nodiscard]] QJsonValue jsonOf(QVariant const& value)
    {
        switch (value.typeId())
        {
            case QMetaType::Bool: return value.toBool();
            case QMetaType::Int:
            case QMetaType::UInt:
            case QMetaType::LongLong:
            case QMetaType::ULongLong: return value.toLongLong();
            case QMetaType::Double:
            case QMetaType::Float: return value.toDouble();
            case QMetaType::QString: return value.toString();
            default: return QJsonValue::Undefined;
        }
    }

    [[nodiscard]] QJsonObject propertiesOf(QQuickItem const& item)
    {
        auto props = QJsonObject {};
        for (auto const* const name: GeometryDumpProperties)
            if (auto const json = jsonOf(item.property(name)); !json.isUndefined())
                props.insert(QString::fromLatin1(name), json);
        return props;
    }

    /// TextInput.Normal, the one echo mode that shows what is typed.
    constexpr auto NormalEchoMode = 0;

    /// Whether @p item holds what the user typed in secret: a masking field, or one marked so.
    [[nodiscard]] bool isSecret(QQuickItem const& item)
    {
        auto const echoMode = item.property("echoMode");
        return (echoMode.isValid() && echoMode.toInt() != NormalEchoMode) || item.property("secret").toBool();
    }

    void describeItem(QQuickItem& item, QJsonArray& items)
    {
        if (!item.isVisible())
            return;
        if ((!item.objectName().isEmpty() || item.inherits("QQuickAbstractButton")) && !isSecret(item))
        {
            auto const topLeft = item.mapToGlobal(QPointF {});
            items.append(QJsonObject {
                { "name", item.objectName() },
                { "class", QString::fromLatin1(item.metaObject()->className()) },
                { "x", topLeft.x() },
                { "y", topLeft.y() },
                { "width", item.width() },
                { "height", item.height() },
                { "props", propertiesOf(item) },
            });
        }
        for (auto* const child: item.childItems())
            describeItem(*child, items);
    }

    /// The window's root item: the content item's parent, which holds the overlay (popups, menus) too.
    [[nodiscard]] QQuickItem* rootOf(QQuickWindow& window)
    {
        auto* root = window.contentItem();
        while (root && root->parentItem())
            root = root->parentItem();
        return root;
    }
} // namespace

std::optional<std::filesystem::path> geometryDumpFileOf(QString const& value)
{
    if (value.isEmpty())
        return std::nullopt;
    return std::filesystem::path { value.toStdString() };
}

QByteArray describeWindowItems(std::span<QQuickWindow* const> windows)
{
    auto described = QJsonArray {};
    for (auto* const window: windows)
    {
        if (!window || !window->isVisible())
            continue;
        auto items = QJsonArray {};
        if (auto* const root = rootOf(*window))
            describeItem(*root, items);
        described.append(QJsonObject {
            { "title", window->title() },
            { "x", window->x() },
            { "y", window->y() },
            { "width", window->width() },
            { "height", window->height() },
            { "active", window->isActive() },
            { "items", items },
        });
    }
    return QJsonDocument(QJsonObject { { "windows", described } }).toJson(QJsonDocument::Compact);
}

GeometryDump::GeometryDump(std::filesystem::path file,
                           std::chrono::milliseconds interval,
                           WindowSource windows):
    _file { std::move(file) }, _windows { std::move(windows) }
{
    _timer.setInterval(interval);
    connect(&_timer, &QTimer::timeout, this, &GeometryDump::tick);
    _timer.start();
}

void GeometryDump::tick()
{
    auto const windows = _windows();
    auto dump = describeWindowItems(windows);
    if (dump == _written)
        return;
    auto file = QSaveFile { QString::fromStdString(_file.string()) };
    // What a window shows is the user's: the file is theirs alone, whatever the umask says.
    if (!file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(dump) != dump.size() || !file.commit())
        return; // A test reading it times out naming what it waited for; the app carries on.
    _written = std::move(dump);
}

} // namespace contour::window
