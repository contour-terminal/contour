// SPDX-License-Identifier: Apache-2.0
#include <contour/window/GeometryDump.hpp>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileDevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>

#include <QtTest/QTest>

using contour::window::describeWindowItems;
using contour::window::GeometryDump;
using contour::window::geometryDumpFileOf;

namespace
{
/// A window holding a named row (with a title and a kind), an unnamed item, and a hidden named item with a
/// named child.
struct Scene
{
    std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();

    Scene()
    {
        window->setGeometry(100, 50, 400, 300);
        auto* const row = new QQuickItem(window->contentItem());
        row->setObjectName("sidebarRow");
        row->setPosition({ 10, 20 });
        row->setSize({ 200, 24 });
        row->setProperty("title", QStringLiteral("build"));
        row->setProperty("kind", 1);
        row->setProperty("unlisted", QStringLiteral("not a dumped property"));
        new QQuickItem(window->contentItem());
        auto* const hidden = new QQuickItem(window->contentItem());
        hidden->setObjectName("hidden");
        hidden->setVisible(false);
        auto* const inside = new QQuickItem(hidden);
        inside->setObjectName("insideHidden");
        window->show();
    }
};

[[nodiscard]] QJsonArray itemsOf(QByteArray const& dump)
{
    auto const windows = QJsonDocument::fromJson(dump).object().value("windows").toArray();
    REQUIRE(windows.size() == 1);
    return windows.first().toObject().value("items").toArray();
}
} // namespace

TEST_CASE("a geometry dump lists the visible named items with their screen rectangles",
          "[contour][gui][geometrydump]")
{
    auto const scene = Scene {};
    auto const windows = std::array { scene.window.get() };
    auto const items = itemsOf(describeWindowItems(windows));

    REQUIRE(items.size() == 1); // neither the unnamed item nor the hidden one, nor what the hidden one holds
    auto const row = items.first().toObject();
    CHECK(row.value("name").toString() == "sidebarRow");
    auto const topLeft = scene.window->contentItem()->childItems().first()->mapToGlobal(QPointF {});
    CHECK(row.value("x").toDouble() == topLeft.x());
    CHECK(row.value("y").toDouble() == topLeft.y());
    CHECK(row.value("width").toDouble() == 200);
    CHECK(row.value("height").toDouble() == 24);
    auto const props = row.value("props").toObject();
    CHECK(props.value("title").toString() == "build");
    CHECK(props.value("kind").toInteger() == 1);
    CHECK_FALSE(props.contains("unlisted"));
}

TEST_CASE("a geometry dump skips a window that is not shown, and a null one", "[contour][gui][geometrydump]")
{
    auto const scene = Scene {};
    scene.window->hide();
    auto const windows = std::array<QQuickWindow*, 2> { scene.window.get(), nullptr };
    CHECK(
        QJsonDocument::fromJson(describeWindowItems(windows)).object().value("windows").toArray().isEmpty());
}

TEST_CASE("the geometry dump writes the file and rewrites it when the scene changes",
          "[contour][gui][geometrydump]")
{
    auto const scene = Scene {};
    auto const dir = QTemporaryDir {};
    REQUIRE(dir.isValid());
    auto const path = dir.filePath("geometry.json");
    auto const dump = GeometryDump { path.toStdString(), std::chrono::milliseconds { 10 }, [&scene] {
                                        return std::vector<QQuickWindow*> { scene.window.get() };
                                    } };
    auto const contents = [&path] {
        auto file = QFile { path };
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
    };

    REQUIRE(QTest::qWaitFor([&] { return !contents().isEmpty(); }, 5000));
    CHECK(itemsOf(contents()).first().toObject().value("width").toDouble() == 200);

    scene.window->contentItem()->childItems().first()->setWidth(150);
    REQUIRE(QTest::qWaitFor(
        [&] { return itemsOf(contents()).first().toObject().value("width").toDouble() == 150; }, 5000));
}

TEST_CASE("a geometry dump leaves out what the user types in secret", "[contour][gui][geometrydump]")
{
    auto const scene = Scene {};
    // A field that masks what is typed (an askpass secret, TextInput.Password = 2), and an item marked
    // secret.
    auto* const masked = new QQuickItem(scene.window->contentItem());
    masked->setObjectName("askpassSecret");
    masked->setProperty("echoMode", 2);
    masked->setProperty("text", QStringLiteral("hunter2"));
    auto* const marked = new QQuickItem(scene.window->contentItem());
    marked->setObjectName("token");
    marked->setProperty("secret", true);
    marked->setProperty("text", QStringLiteral("s3cr3t"));
    // A field that shows what is typed is listed as any other.
    auto* const plain = new QQuickItem(scene.window->contentItem());
    plain->setObjectName("username");
    plain->setProperty("echoMode", 0);
    plain->setProperty("text", QStringLiteral("alice"));

    auto const windows = std::array { scene.window.get() };
    auto const dump = describeWindowItems(windows);
    CHECK_FALSE(dump.contains("hunter2"));
    CHECK_FALSE(dump.contains("s3cr3t"));
    auto const items = itemsOf(dump);
    REQUIRE(items.size() == 2);
    CHECK(items.at(1).toObject().value("name").toString() == "username");
    CHECK(items.at(1).toObject().value("props").toObject().value("text").toString() == "alice");
}

TEST_CASE("the geometry dump is on only for a file named", "[contour][gui][geometrydump]")
{
    CHECK_FALSE(geometryDumpFileOf(QString {}).has_value());
    CHECK(geometryDumpFileOf(QStringLiteral("/tmp/geometry.json"))
          == std::filesystem::path { "/tmp/geometry.json" });
}

#ifndef _WIN32
TEST_CASE("the geometry dump's file is its owner's alone", "[contour][gui][geometrydump]")
{
    auto const scene = Scene {};
    auto const dir = QTemporaryDir {};
    REQUIRE(dir.isValid());
    auto const path = dir.filePath("geometry.json");
    auto const dump = GeometryDump { path.toStdString(), std::chrono::milliseconds { 10 }, [&scene] {
                                        return std::vector<QQuickWindow*> { scene.window.get() };
                                    } };
    REQUIRE(QTest::qWaitFor([&] { return QFile::exists(path); }, 5000));
    auto const others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                        | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    CHECK((QFile::permissions(path) & others) == QFileDevice::Permissions {});
    CHECK(QFile::permissions(path).testFlag(QFileDevice::ReadOwner));
}
#endif
