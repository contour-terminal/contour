// vim:syntax=qml
// Right-click context menu for a tab: rename, color, and close operations.
import QtQuick
import QtQuick.Controls

Menu {
    id: menu

    // Force the in-scene (item) popup rather than a native OS menu. Since Qt 6.8 a Controls Menu
    // defaults to a native platform menu where one exists — on Windows that native menu could not
    // represent this menu's custom surface, and it came up EMPTY. Rendering in-scene makes Windows
    // behave like Linux (which has no native popup menu and already used this path); the app-pinned
    // Fusion style then gives the menu an opaque, themed surface with readable item text on every
    // platform, so no custom background/palette override is needed here (the ApplicationWindow is
    // transparent, but Fusion's own menu background is opaque).
    popupType: Popup.Item

    required property var controller
    required property int tabIndex

    // Raised when the user picks "Rename"; the TabButton starts its inline rename field.
    signal renameRequested()

    // Raised when the user picks "Choose Color…"; the TabItem opens its TabColorFlyout. The flyout is
    // NOT a child of this menu: the keyboard (the SetTabColor action) must be able to open it with no
    // menu in sight, so it lives on the TabItem and both entry points ask that one instance to open.
    //
    // Raised on CLOSE, not on trigger. The flyout is a modal popup over the same overlay this menu
    // grabs, so opening it while the menu is still dismissing hands it a grab the menu then takes back,
    // leaving the hex field without keyboard focus. Announcing once the menu is actually gone keeps that
    // knowledge here — where the dismissal happens — instead of making the TabItem defer a turn and hope.
    signal colorRequested()

    property bool colorPending: false

    onClosed: {
        if (menu.colorPending) {
            menu.colorPending = false;
            menu.colorRequested();
        }
    }

    MenuItem {
        text: qsTr("Choose Color…")
        onTriggered: menu.colorPending = true
    }

    MenuItem {
        text: qsTr("Rename…")
        onTriggered: menu.renameRequested()
    }

    MenuSeparator {}

    MenuItem {
        text: qsTr("Close")
        onTriggered: menu.controller.closeTabAtIndex(menu.tabIndex)
    }
    MenuItem {
        text: qsTr("Close Other Tabs")
        onTriggered: menu.controller.closeOtherTabs(menu.tabIndex)
    }
    MenuItem {
        text: qsTr("Close Tabs to the Right")
        onTriggered: menu.controller.closeTabsToRight(menu.tabIndex)
    }
}
