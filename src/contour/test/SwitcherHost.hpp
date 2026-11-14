// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A stand-in for the WindowController a switcher is presented in, and the QML window that hosts
/// Switcher.qml and the workspace chip beside it, as Main.qml and the title bar do: what the switcher's
/// offscreen tests drive.

#include <contour/test/PopupWaits.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/window/LocationChip.hpp>
#include <contour/window/LocationChipSteps.hpp>
#include <contour/window/SwitcherController.hpp>
#include <contour/window/SwitcherPresenter.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QRectF>
#include <QtCore/QVariant>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>

namespace contour::test
{

/// Mirrors WindowController's switcher hosting: presentSwitcher(), withdrawSwitcher(), closeSwitcher(), the
/// chip's dropdown state, and showWorkspaceDropdown(), which a test answers. Presented in by the production
/// SwitcherPresenter.
class MockSwitcherHost: public QObject, public window::SwitcherHost
{
    Q_OBJECT
    Q_PROPERTY(QObject* switcher READ switcher NOTIFY switcherChanged)
    Q_PROPERTY(bool isWorkspaceDropdownOpen READ isWorkspaceDropdownOpen NOTIFY switcherChanged)
    Q_PROPERTY(QString workspaceChipDescription READ workspaceChipDescription NOTIFY switcherChanged)
    // The location chip of an in-process window: the workspace alone, no daemon segment, no dot.
    Q_PROPERTY(QString chipDaemonName READ chipDaemonName CONSTANT)
    Q_PROPERTY(int chipDaemonDot READ chipDaemonDot CONSTANT)
    Q_PROPERTY(QString chipStateGlyph READ chipStateGlyph CONSTANT)
    Q_PROPERTY(int hostChipState READ hostChipState CONSTANT)
    Q_PROPERTY(QVariantList chipFitSteps READ chipFitSteps CONSTANT)
    Q_PROPERTY(QString chipAccessibleName READ chipAccessibleName CONSTANT)
    Q_PROPERTY(QString chipAccessibleDescription READ workspaceChipDescription NOTIFY switcherChanged)

  public:
    [[nodiscard]] static QString chipDaemonName() { return {}; }
    [[nodiscard]] static int chipDaemonDot() { return static_cast<int>(window::DaemonDot::Hidden); }
    [[nodiscard]] static QString chipStateGlyph()
    {
        return QString::fromUtf8(window::daemonStateGlyph(window::HostChipState::Connected));
    }
    [[nodiscard]] static QVariantList chipFitSteps() { return window::locationChipStepList(); }
    [[nodiscard]] static int hostChipState() { return static_cast<int>(window::HostChipState::Connected); }
    [[nodiscard]] static QString chipAccessibleName() { return QStringLiteral("Workspace contour"); }
    Q_INVOKABLE [[nodiscard]] static QString chipToolTip(int) { return QStringLiteral("Switch workspace"); }
    Q_INVOKABLE [[nodiscard]] static QString chipDaemonText(int) { return {}; }
    Q_INVOKABLE [[nodiscard]] static QString chipStatusText(int) { return {}; }
    Q_INVOKABLE [[nodiscard]] static QString chipWorkspaceText(QString const& workspace, int)
    {
        return workspace;
    }

    /// Mirrors WindowController::presentSwitcher().
    void presentSwitcher(window::SwitcherController* switcher, std::optional<QRectF> anchor) override
    {
        _switcher = switcher;
        _anchor = anchor;
        emit switcherChanged();
        emit switcherRequested(anchor ? QVariant { *anchor } : QVariant {});
    }

    /// Mirrors WindowController::withdrawSwitcher().
    void withdrawSwitcher() override
    {
        if (!_switcher)
            return;
        letGo();
        emit switcherWithdrawn();
    }

    [[nodiscard]] QObject& switcherHostObject() noexcept override { return *this; }

    /// Mirrors WindowController::closeSwitcher(): a popup that closed while still presented was dismissed.
    Q_INVOKABLE void closeSwitcher()
    {
        ++closes;
        auto* const dismissed = _switcher.data();
        if (dismissed == nullptr)
            return;
        letGo();
        dismissed->cancel();
    }

    /// Mirrors WindowController::showWorkspaceDropdown(): counted, and answered by @c onDropdown.
    Q_INVOKABLE void showWorkspaceDropdown(QRectF const& chip)
    {
        ++dropdowns;
        if (onDropdown)
            onDropdown(chip);
    }

    [[nodiscard]] QObject* switcher() const { return _switcher.data(); }
    [[nodiscard]] bool isWorkspaceDropdownOpen() const { return _switcher && _anchor.has_value(); }
    [[nodiscard]] QString workspaceChipDescription() const
    {
        return isWorkspaceDropdownOpen() ? QStringLiteral("expanded") : QStringLiteral("collapsed");
    }

    std::function<void(QRectF)> onDropdown; ///< What the chip's click does; nothing when unset.
    int dropdowns = 0;                      ///< How often the chip asked for its dropdown.
    int closes = 0;                         ///< How often closeSwitcher() ran.

  signals:
    void switcherChanged();
    void switcherRequested(QVariant anchor);
    void switcherWithdrawn();

  private:
    void letGo()
    {
        _switcher = nullptr;
        _anchor.reset();
        emit switcherChanged();
    }

    QPointer<window::SwitcherController> _switcher;
    std::optional<QRectF> _anchor;
};

/// A window hosting Switcher.qml and a workspace chip, over a MockSwitcherHost: Main.qml's wiring and the
/// title bar's chip, without the rest of the window.
struct SwitcherWindow
{
    std::unique_ptr<QObject> root;
    QQuickWindow* window = nullptr;
    QObject* popup = nullptr;
    QQuickItem* chip = nullptr;
    QQuickItem* editor = nullptr; ///< The stand-in for the sidebar's rename field.

    SwitcherWindow(QQmlEngine& engine, MockSwitcherHost& host)
    {
        QQmlComponent component(&engine);
        component.setData(R"(import QtQuick
import QtQuick.Window
import Contour.Ui
Window {
    id: hostWindow
    width: 1024; height: 640; visible: true
    required property var switcherHost
    property int focusRestores: 0
    property alias switcherItem: popup
    property alias chipItem: chip
    property alias editorItem: editor
    // As the real window does: the keyboard goes back to the terminal.
    function restoreTerminalFocus() { hostWindow.focusRestores++; terminal.forceActiveFocus(); }
    Item { id: terminal; objectName: "terminal"; focus: true }
    // Where a sidebar row's Rename… types: what a row menu's pick may focus.
    TextInput { id: editor; objectName: "sidebarEditor"; x: 40; y: 600; width: 200; height: 20 }
    WorkspaceChip {
        id: chip
        x: 40; y: 8
        sidebar: ({ currentWorkspaceTitle: "contour", workspaceChipMarked: false,
                    workspaceChipAccessibleName: "contour", workspaceChipGlyph: 0 })
        controller: hostWindow.switcherHost
        isOpen: hostWindow.switcherHost.isWorkspaceDropdownOpen
        onClicked: hostWindow.switcherHost.showWorkspaceDropdown(chip.dropdownAnchor())
    }
    Switcher {
        id: popup
        objectName: "switcher"
        controller: hostWindow.switcherHost
        window: hostWindow
    }
}
)",
                          QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SwitcherTestHost.qml")));
        while (component.status() == QQmlComponent::Loading)
            QCoreApplication::processEvents();
        INFO(component.errorString().toStdString());
        REQUIRE(component.isReady());
        root.reset(component.createWithInitialProperties(
            QVariantMap { { QStringLiteral("switcherHost"), QVariant::fromValue(&host) } }));
        REQUIRE(root != nullptr);
        window = qobject_cast<QQuickWindow*>(root.get());
        REQUIRE(window != nullptr);
        popup = root->property("switcherItem").value<QObject*>();
        editor = root->property("editorItem").value<QQuickItem*>();
        chip = root->property("chipItem").value<QQuickItem*>();
        REQUIRE(popup != nullptr);
        REQUIRE(chip != nullptr);
        REQUIRE(QTest::qWaitForWindowExposed(window));
    }

    [[nodiscard]] bool isOpen() const { return popup->property("visible").toBool(); }

    /// @return The popup's part named @p name, or null.
    [[nodiscard]] QQuickItem* part(QString const& name) const { return popup->findChild<QQuickItem*>(name); }

    /// @return The realized delegate of list row @p index, or null.
    [[nodiscard]] QQuickItem* rowAt(int index) const
    {
        auto* list = part(QStringLiteral("switcherList"));
        if (list == nullptr)
            return nullptr;
        QQuickItem* row = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, index));
        return row;
    }

    /// @return The scene rect of @p item.
    [[nodiscard]] static QRectF sceneRectOf(QQuickItem const& item)
    {
        return item.mapRectToScene(QRectF { 0, 0, item.width(), item.height() });
    }
};

} // namespace contour::test
