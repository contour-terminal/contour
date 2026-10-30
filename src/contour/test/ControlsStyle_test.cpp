// SPDX-License-Identifier: Apache-2.0
//
// The controls style a run draws under: the one its variant names (CONTOUR_TEST_CONTROLS_STYLE), found where
// QQuickStyle looks for it.

#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/TestEnvironment.hpp>

#include <core/Environment.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>

#include <QtQuickControls2/QQuickStyle>

TEST_CASE("controlsStyleQmldirs names a style of Qt's own, then a module of the style's name",
          "[contour][gui]")
{
    CHECK(contour::test::controlsStyleQmldirs("FluentWinUI3")
          == std::array<std::string, 2> { "QtQuick/Controls/FluentWinUI3/qmldir", "FluentWinUI3/qmldir" });
    CHECK(contour::test::controlsStyleQmldirs("ContourTui")
          == std::array<std::string, 2> { "QtQuick/Controls/ContourTui/qmldir", "ContourTui/qmldir" });
}

TEST_CASE("a run draws its controls under the style its variant names", "[contour][gui][qml]")
{
    // A control resolves the style: until one is created, QQuickStyle only reports what was asked for. A
    // style Qt could not load fails the import here, and a fallback it took instead fails the run-wide QML
    // gate.
    auto engine = QQmlEngine {};
    // Contour's own styles draw from the chrome's tokens, as the app's controls do.
    contour::test::installChromeStyle(engine);
    auto component = QQmlComponent { &engine };
    component.setData(
        QByteArrayLiteral("import QtQuick\nimport QtQuick.Controls\nButton { text: \"probe\" }\n"), QUrl {});
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto const button = std::unique_ptr<QObject> { component.create() };
    REQUIRE(button != nullptr);

    CHECK(QQuickStyle::name().toStdString() == contour::test::controlsStyleFor(core::LiveEnvironment {}));
}
