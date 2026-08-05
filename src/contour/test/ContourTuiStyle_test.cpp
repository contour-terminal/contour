// SPDX-License-Identifier: Apache-2.0
#include <contour/config/UiStyle.h>
#include <contour/UiStyleProvider.h>
#include <contour/test/QmlChromeStyle.h>

#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtGui/QFont>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>

// The ContourTui Qt Quick Controls style is selected by name, once per process, before the first
// control exists (ContourGuiApp). That makes it untestable by *selecting* it here -- this binary has
// already pinned Fusion in test_main.cpp, and Qt offers no way to re-resolve a style afterwards.
// (Qt 6.11's QQuickStyle exposes only name/setStyle/setFallbackStyle; there is no availableStyles()
// to ask, either.)
//
// What does break, and what these cover, is the packaging and the style files themselves.

TEST_CASE("The ContourTui style module is packaged where QQuickStyle can find it", "[contour][gui][style]")
{
    // A Qt Quick Controls style is resolved by looking for a module named after the style on the
    // import path. :/qt/qml is on that path by default, so the module has to land at
    // :/qt/qml/ContourTui with its qmldir beside the controls. Get the CMake wiring wrong -- a missing
    // RESOURCE_PREFIX, an un-aliased subdirectory, a module that never links -- and QQuickStyle
    // silently falls back, leaving the user with `ui_style: terminal` and unstyled controls, and no
    // diagnostic anywhere.
    CHECK(QFile::exists(QStringLiteral(":/qt/qml/ContourTui/qmldir")));
    CHECK(QFile::exists(QStringLiteral(":/qt/qml/ContourTui/ToolButton.qml")));
}

TEST_CASE("Every ContourTui control loads without errors", "[contour][gui][style]")
{
    // The style files are never instantiated by anything else in this binary -- Fusion is pinned -- so
    // without this they are only ever compiled, and a bad property name or a missing import would first
    // surface as an unstyled control in front of a user who set `ui_style: terminal`.
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, contour::config::UiStyle::Terminal);

    auto const controls = std::array {
        "Button",     "CheckBox", "ComboBox",      "ItemDelegate", "Label",
        "Menu",       "MenuItem", "MenuSeparator", "Popup",        "ScrollBar",
        "ScrollView", "Switch",   "TextField",     "ToolButton",   "ToolTip",
    };

    for (auto const* name: controls)
    {
        auto const url = QStringLiteral("qrc:/qt/qml/ContourTui/%1.qml").arg(QLatin1StringView(name));
        INFO("control: " << name);
        QQmlComponent component(&engine, QUrl(url));
        INFO("errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());

        std::unique_ptr<QObject> instance(component.create());
        CHECK(instance != nullptr);
    }
}

TEST_CASE("A ContourTui control is sized in whole character cells", "[contour][gui][style]")
{
    // Loading a style file directly is the closest one process can get to seeing the style in use: it
    // proves the file is valid QML, that it resolves `chromeStyle`, and that it takes its metrics from
    // the tokens rather than from a hardcoded size.
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, contour::config::UiStyle::Terminal);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/ContourTui/ToolButton.qml")));
    INFO("ToolButton errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    std::unique_ptr<QObject> button(component.create());
    REQUIRE(button != nullptr);

    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::UiStyleProvider*>();
    REQUIRE(provider != nullptr);
    REQUIRE(provider->cellHeight() > 0.0);

    // An empty button is exactly its background: one cell tall, so it sits inside a one-row strip.
    CHECK(button->property("implicitHeight").toReal() == provider->controlHeight());
    CHECK(provider->controlHeight() == provider->cellHeight());

    // And it is set in the chrome font rather than the platform UI font -- that is what puts its glyph
    // on the same grid as the terminal content below.
    CHECK(button->property("font").value<QFont>().family() == provider->font().family());
}

TEST_CASE("A ContourTui menu grows to fit its longest item", "[contour][gui][style]")
{
    // A menu whose width came from its background's minimum would elide every entry longer than that
    // minimum -- and the entries are the menu. So this pins that the popup takes its width from the
    // widest item, which is what `contentWidth` on the template is for.
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, contour::config::UiStyle::Terminal);

    QQmlComponent menuComponent(&engine, QUrl(QStringLiteral("qrc:/qt/qml/ContourTui/Menu.qml")));
    INFO("Menu errors: " << menuComponent.errorString().toStdString());
    REQUIRE(menuComponent.isReady());
    std::unique_ptr<QObject> menu(menuComponent.create());
    REQUIRE(menu != nullptr);

    QQmlComponent itemComponent(&engine, QUrl(QStringLiteral("qrc:/qt/qml/ContourTui/MenuItem.qml")));
    INFO("MenuItem errors: " << itemComponent.errorString().toStdString());
    REQUIRE(itemComponent.isReady());

    // One short entry and one far longer than the background's minimum width.
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

    auto const menuWidth = menu->property("implicitWidth").toReal();
    INFO("menu implicitWidth=" << menuWidth << " widest item=" << widest);
    // The menu must be at least as wide as its widest entry; anything less clips that entry.
    CHECK(menuWidth >= widest);
}

TEST_CASE("A ContourTui menu can build the row a sub-menu needs", "[contour][gui][style]")
{
    // Menu.addMenu() does not create a plain item: QQuickMenu::insertMenu() goes through
    // beginCreateItem(), which instantiates the STYLE's `delegate`. A style that leaves it null makes
    // beginCreateItem() return nullptr and insertItem() then quietly drop the sub-menu -- so "New Tab
    // with Profile" and the whole "Tab Bar" sub-menu vanish from the title bar's context menu, with
    // their separators left behind and no diagnostic anywhere.
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, contour::config::UiStyle::Terminal);

    QQmlComponent menuComponent(&engine, QUrl(QStringLiteral("qrc:/qt/qml/ContourTui/Menu.qml")));
    INFO("Menu errors: " << menuComponent.errorString().toStdString());
    REQUIRE(menuComponent.isReady());
    std::unique_ptr<QObject> menu(menuComponent.create());
    REQUIRE(menu != nullptr);

    auto* delegate = menu->property("delegate").value<QQmlComponent*>();
    REQUIRE(delegate != nullptr);

    // And it has to instantiate, which is the other half of what beginCreateItem() does.
    INFO("delegate errors: " << delegate->errorString().toStdString());
    std::unique_ptr<QObject> row(delegate->create());
    REQUIRE(row != nullptr);
    // A sub-menu row is marked as one; without the arrow it is indistinguishable from a command.
    CHECK(row->property("arrow").value<QQuickItem*>() != nullptr);
}

TEST_CASE("Every ContourTui popup surface declares an overlay scrim", "[contour][gui][style]")
{
    // A modal popup is dimmed by the STYLE's `T.Overlay.modal` attached declaration: QQuickOverlay
    // creates no background item at all when it is unset. The command palette, the save-layout
    // prompt, the tab color flyout and the status-line item dialog are all `Popup { modal: true }`,
    // so a style that omits it leaves each of them floating over a fully-lit window that still
    // swallows every click -- the user cannot tell what is live and gets no feedback for clicking it.
    //
    // Asserted against the source because the declaration is an attached property of a type Qt does
    // not export; there is no public handle to read it back from on the instance.
    for (auto const* control: std::array { "Popup", "Menu" })
    {
        auto file = QFile(QStringLiteral(":/qt/qml/ContourTui/%1.qml").arg(QLatin1StringView(control)));
        INFO("control: " << control);
        REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
        auto const source = QString::fromUtf8(file.readAll());
        CHECK(source.contains(QStringLiteral("T.Overlay.modal:")));
        CHECK(source.contains(QStringLiteral("T.Overlay.modeless:")));
    }
}

TEST_CASE("A ContourTui text field shows its placeholder", "[contour][gui][style]")
{
    // T.TextField carries `placeholderText` but draws nothing for it -- rendering it is the style's
    // job. A style that only sets `placeholderTextColor` hands the user an unlabeled empty box: the
    // command palette with no "Type to search commands…", a color field with no "#RRGGBB".
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, contour::config::UiStyle::Terminal);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/ContourTui/TextField.qml")));
    INFO("TextField errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    std::unique_ptr<QQuickItem> field(qobject_cast<QQuickItem*>(component.create()));
    REQUIRE(field != nullptr);

    auto const hint = QStringLiteral("Type to search commands…");
    field->setProperty("placeholderText", hint);

    auto const hintShowing = [&field, &hint] {
        return std::ranges::any_of(field->childItems(), [&hint](QQuickItem const* child) {
            return child->property("text").toString() == hint && child->isVisible();
        });
    };

    CHECK(hintShowing());

    // And it steps aside the moment there is real text, rather than printing over it.
    field->setProperty("text", QStringLiteral("close"));
    CHECK_FALSE(hintShowing());
}

TEST_CASE("A ContourTui scrollbar keeps a grabbable handle", "[contour][gui][style]")
{
    // QQuickScrollBar defaults minimumSize to 0 and then sizes the handle at `size * trackLength` with
    // no floor, so a long enough list shrinks it to a sub-pixel sliver: the trough is still drawn, but
    // there is nothing left to drag and the settings page can only be scrolled by wheel.
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, contour::config::UiStyle::Terminal);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/ContourTui/ScrollBar.qml")));
    INFO("ScrollBar errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    std::unique_ptr<QQuickItem> bar(qobject_cast<QQuickItem*>(component.create()));
    REQUIRE(bar != nullptr);

    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::UiStyleProvider*>();
    REQUIRE(provider != nullptr);
    REQUIRE(provider->cellHeight() > 0.0);

    // Forty rows of track showing a two-hundredth of the content: without a floor that is a handle
    // half a pixel tall.
    bar->setWidth(provider->cellWidth());
    bar->setHeight(provider->cellHeight() * 40);
    bar->setProperty("size", 0.005);

    auto* handle = bar->property("contentItem").value<QQuickItem*>();
    REQUIRE(handle != nullptr);
    INFO("minimumSize=" << bar->property("minimumSize").toReal() << " handle=" << handle->height());
    CHECK(handle->height() >= provider->cellHeight());
}

TEST_CASE("A ContourTui combo box popup is sized before it has a window", "[contour][gui][style]")
{
    // The popup clamps its height against the window's, minus its own margins. QQuickPopup defaults
    // those margins to -1 ("do not push me back inside the window"), which subtracted would ADD two
    // pixels where a margin was meant to be reserved -- and, before the box is parented into a window
    // at all, `Window.height` is 0 and the whole clamp collapses the popup to a 2px sliver.
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, contour::config::UiStyle::Terminal);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/ContourTui/ComboBox.qml")));
    INFO("ComboBox errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    std::unique_ptr<QObject> combo(component.create());
    REQUIRE(combo != nullptr);

    auto* popup = combo->property("popup").value<QObject*>();
    REQUIRE(popup != nullptr);

    CHECK(popup->property("topMargin").toReal() > 0.0);
    CHECK(popup->property("bottomMargin").toReal() > 0.0);

    // ...and the margins being real numbers is exactly what makes the window clamp dangerous before
    // the box has a window: `0 - topMargin - bottomMargin` is NEGATIVE, so an unguarded Math.min
    // would size the popup to that. Its content is empty here, so all that is left to see is the
    // border -- but a positive height is the whole assertion, and the unguarded form has none.
    CHECK(popup->property("height").toReal() > 0.0);
}
