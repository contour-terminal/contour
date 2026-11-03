// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ControlsStyleProbe.hpp>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("isControlsStyleInstalled looks for the style's qmldir beneath the import paths",
          "[controlsstyle][platform]")
{
    auto const dir = QTemporaryDir {};
    REQUIRE(dir.isValid());
    auto const makeQmldir = [&](QString const& relative) {
        REQUIRE(QDir {}.mkpath(dir.filePath(relative)));
        auto file = QFile { dir.filePath(relative + QStringLiteral("/qmldir")) };
        REQUIRE(file.open(QIODevice::WriteOnly));
    };
    makeQmldir(QStringLiteral("QtQuick/Controls/FakeNative"));
    makeQmldir(QStringLiteral("FakeOwn"));

    auto const importPaths = QStringList { dir.filePath(QStringLiteral("elsewhere")), dir.path() };

    CHECK(contour::platform::isControlsStyleInstalled("FakeNative", importPaths));
    CHECK(contour::platform::isControlsStyleInstalled("FakeOwn", importPaths));
    CHECK_FALSE(contour::platform::isControlsStyleInstalled("Windows", importPaths));
    CHECK_FALSE(contour::platform::isControlsStyleInstalled("FakeOwn", QStringList {}));
}

TEST_CASE("isControlsStyleInstalled finds a style in the resource tree named by a qrc URL",
          "[controlsstyle][platform]")
{
    // QQmlEngine::importPathList() names the compiled-in modules as `qrc:/qt/qml`, which QFileInfo does not
    // resolve; ContourLinux is compiled into this binary.
    CHECK(contour::platform::isControlsStyleInstalled("ContourLinux",
                                                      QStringList { QStringLiteral("qrc:/qt/qml") }));
    CHECK(contour::platform::isControlsStyleInstalled("ContourLinux",
                                                      QStringList { QStringLiteral(":/qt/qml") }));
    CHECK_FALSE(contour::platform::isControlsStyleInstalled("NoSuchStyle",
                                                            QStringList { QStringLiteral("qrc:/qt/qml") }));
}
