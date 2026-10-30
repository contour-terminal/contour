// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/ChromeTokenRows.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QRectF>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtGui/QFontMetricsF>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>

// The ContourLinux style, as ContourTuiStyle_test.cpp covers ContourTui: the style is selected once per
// process (test_main.cpp), so these cases load the style files directly, under the ContourLinux row's
// tokens. The contour_gui_test_contourlinux ctest runs the QML suite with the style selected.

namespace
{
/// Installs the ContourLinux row's tokens on @p engine.
void installLinuxChrome(QQmlEngine& engine)
{
    auto setup = contour::test::defaultChromeSetup(contour::config::UiStyle::Native);
    setup.row = contour::config::ChromeStyle::ContourLinux;
    contour::test::installChromeStyle(engine, setup);
}

/// @return The URL of the ContourLinux control @p name.
[[nodiscard]] QUrl controlUrl(char const* name)
{
    return QUrl { QStringLiteral("qrc:/qt/qml/ContourLinux/%1.qml").arg(QLatin1StringView(name)) };
}

/// Loads and instantiates the ContourLinux control @p name.
[[nodiscard]] std::unique_ptr<QObject> createControl(QQmlEngine& engine, char const* name)
{
    QQmlComponent component(&engine, controlUrl(name));
    INFO(name << " errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto instance = std::unique_ptr<QObject>(component.create());
    REQUIRE(instance != nullptr);
    return instance;
}

/// @return Token @p name of group @p group of @p engine's `chromeStyle`.
[[nodiscard]] qreal tokenOf(QQmlEngine const& engine, char const* group, char const* name)
{
    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::window::UiStyleProvider*>();
    REQUIRE(provider != nullptr);
    auto const* tokens = provider->property(group).value<QObject*>();
    REQUIRE(tokens != nullptr);
    return tokens->property(name).toReal();
}

/// @return The radius of @p control's background.
[[nodiscard]] qreal backgroundRadiusOf(QObject const& control)
{
    auto const* background = control.property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    return background->property("radius").toReal();
}
} // namespace

TEST_CASE("The ContourLinux style module is packaged where QQuickStyle can find it",
          "[contour][gui][style][contourlinux]")
{
    // Get the CMake wiring wrong and QQuickStyle silently falls back to Fusion, with no diagnostic.
    CHECK(QFile::exists(QStringLiteral(":/qt/qml/ContourLinux/qmldir")));
    CHECK(QFile::exists(QStringLiteral(":/qt/qml/ContourLinux/ToolButton.qml")));
}

TEST_CASE("Every ContourLinux control loads without errors", "[contour][gui][style][contourlinux]")
{
    QQmlEngine engine;
    installLinuxChrome(engine);

    auto const controls = std::array {
        "Button",     "CheckBox", "ComboBox",      "ItemDelegate", "Label",
        "Menu",       "MenuItem", "MenuSeparator", "Popup",        "ScrollBar",
        "ScrollView", "Switch",   "TextField",     "ToolButton",   "ToolTip",
    };
    for (auto const* name: controls)
    {
        INFO("control: " << name);
        auto const instance = createControl(engine, name);
        CHECK(instance != nullptr);
    }
}

TEST_CASE("A ContourLinux control takes the kit's ContourLinux metrics",
          "[contour][gui][style][contourlinux]")
{
    QQmlEngine engine;
    installLinuxChrome(engine);
    auto const& row =
        contour::config::ChromeTokenRows[static_cast<size_t>(contour::config::ChromeStyle::ContourLinux)];

    auto const button = createControl(engine, "ToolButton");
    CHECK(button->property("implicitHeight").toReal() == tokenOf(engine, "size", "control"));
    CHECK(tokenOf(engine, "size", "control") == row.size.control);
    CHECK(backgroundRadiusOf(*button) == tokenOf(engine, "radii", "control"));
    CHECK(backgroundRadiusOf(*button) == row.radii.control);

    auto const menu = createControl(engine, "Menu");
    CHECK(backgroundRadiusOf(*menu) == tokenOf(engine, "radii", "menu"));
    CHECK(backgroundRadiusOf(*menu) == row.radii.menu);

    auto const menuItem = createControl(engine, "MenuItem");
    CHECK(backgroundRadiusOf(*menuItem) == tokenOf(engine, "radii", "menuRow"));
    CHECK(backgroundRadiusOf(*menuItem) == row.radii.menuRow);

    auto const popup = createControl(engine, "Popup");
    CHECK(backgroundRadiusOf(*popup) == tokenOf(engine, "radii", "popover"));
    CHECK(backgroundRadiusOf(*popup) == row.radii.popover);
}

TEST_CASE("A ContourLinux menu grows to fit its longest item", "[contour][gui][style][contourlinux]")
{
    QQmlEngine engine;
    installLinuxChrome(engine);
    auto const menu = createControl(engine, "Menu");

    QQmlComponent itemComponent(&engine, controlUrl("MenuItem"));
    REQUIRE(itemComponent.isReady());
    auto const labels =
        std::array { QStringLiteral("Close"), QStringLiteral("Close Tabs to the Right and Then Some More") };
    auto widest = 0.0;
    for (auto const& label: labels)
    {
        auto* item = qobject_cast<QQuickItem*>(itemComponent.create());
        REQUIRE(item != nullptr);
        item->setProperty("text", label);
        REQUIRE(QMetaObject::invokeMethod(menu.get(), "addItem", Q_ARG(QQuickItem*, item)));
        widest = std::max(widest, item->property("implicitWidth").toReal());
    }
    REQUIRE(widest > 0.0);
    CHECK(menu->property("implicitWidth").toReal() >= widest);
}

TEST_CASE("A ContourLinux menu can build the row a sub-menu needs", "[contour][gui][style][contourlinux]")
{
    // Menu.addMenu() instantiates the style's `delegate`; a null one drops every sub-menu silently.
    QQmlEngine engine;
    installLinuxChrome(engine);
    auto const menu = createControl(engine, "Menu");

    auto* delegate = menu->property("delegate").value<QQmlComponent*>();
    REQUIRE(delegate != nullptr);
    INFO("delegate errors: " << delegate->errorString().toStdString());
    std::unique_ptr<QObject> row(delegate->create());
    REQUIRE(row != nullptr);
    CHECK(row->property("arrow").value<QQuickItem*>() != nullptr);
}

TEST_CASE("Every ContourLinux popup surface declares an overlay scrim", "[contour][gui][style][contourlinux]")
{
    // QQuickOverlay creates no dimming item when the style leaves these unset; the attached property is
    // not readable from the instance, so the source is checked.
    for (auto const* control: std::array { "Popup", "Menu" })
    {
        auto file = QFile(QStringLiteral(":/qt/qml/ContourLinux/%1.qml").arg(QLatin1StringView(control)));
        INFO("control: " << control);
        REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
        auto const source = QString::fromUtf8(file.readAll());
        CHECK(source.contains(QStringLiteral("T.Overlay.modal:")));
        CHECK(source.contains(QStringLiteral("T.Overlay.modeless:")));
    }
}

TEST_CASE("A ContourLinux tooltip shows its text as written", "[contour][gui][style][contourlinux]")
{
    // A tooltip may carry what an application wrote (an OSC 8 URI); as AutoText it would render markup.
    QQmlEngine engine;
    installLinuxChrome(engine);
    auto const tip = createControl(engine, "ToolTip");
    auto const uri = QStringLiteral(R"(http://a/<img src=x>)");
    tip->setProperty("text", uri);

    auto const* content = tip->property("contentItem").value<QQuickItem*>();
    REQUIRE(content != nullptr);
    CHECK(content->property("textFormat").toInt() == Qt::PlainText);
    CHECK(content->property("text").toString() == uri);
}

TEST_CASE("A ContourLinux text field draws its placeholder clear of the caret",
          "[contour][gui][style][contourlinux]")
{
    // The caret stands where the first letter will be typed; a placeholder drawn from there has the caret
    // on its first letter.
    QQmlEngine engine;
    installLinuxChrome(engine);
    auto const field = createControl(engine, "TextField");
    auto const hint = QStringLiteral("Type a command or search…");
    field->setProperty("placeholderText", hint);
    auto const* item = qobject_cast<QQuickItem*>(field.get());
    REQUIRE(item != nullptr);
    auto const children = item->childItems();
    auto const placeholder = std::ranges::find_if(
        children, [&](QQuickItem const* child) { return child->property("text").toString() == hint; });
    REQUIRE(placeholder != children.end());
    auto const caret = field->property("cursorRectangle").toRectF();
    auto const* provider =
        engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(provider != nullptr);
    CHECK((*placeholder)->x() >= caret.x() + caret.width() + provider->property("borderWidth").toReal());
}

TEST_CASE("A ContourLinux combo box fits its longest option, not just the one showing",
          "[contour][gui][style][contourlinux]")
{
    QQmlEngine engine;
    installLinuxChrome(engine);
    auto const combo = createControl(engine, "ComboBox");

    combo->setProperty("model", QStringList { "No", "Always", "Underlined text with a long tail" });
    combo->setProperty("currentIndex", 0);
    auto const font = combo->property("font").value<QFont>();
    auto const longest =
        QFontMetricsF(font).horizontalAdvance(QStringLiteral("Underlined text with a long tail"));

    CHECK(combo->property("modelWidth").toReal() >= longest);
    CHECK(combo->property("implicitWidth").toReal()
          >= longest + combo->property("leftPadding").toReal() + combo->property("rightPadding").toReal());
    auto const* popup = combo->property("popup").value<QObject*>();
    REQUIRE(popup != nullptr);
    CHECK(popup->property("width").toReal() >= longest);
}
