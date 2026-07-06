// vim:syntax=qml
// Right-click context menu for a tab: rename, color, and close operations.
import QtQuick
import QtQuick.Controls

Menu {
    id: menu

    // Force the in-scene (item) popup rather than a native OS menu. Since Qt 6.8 a Controls Menu
    // defaults to a native platform menu where one exists — on Windows that native menu cannot represent
    // this menu's nested TabColorFlyout Popup child, so it came up EMPTY. Rendering in-scene makes
    // Windows behave like Linux (which has no native popup menu and already used this path); the
    // app-pinned Fusion style then gives the menu an opaque, themed surface with readable item text on
    // every platform, so no custom background/palette override is needed here (the ApplicationWindow is
    // transparent, but Fusion's own menu background is opaque).
    popupType: Popup.Item

    required property var controller
    required property int tabIndex

    // Raised when the user picks "Rename"; the TabButton starts its inline rename field.
    signal renameRequested()

    MenuItem {
        text: qsTr("Choose Color…")
        onTriggered: colorFlyout.open()
    }

    // The swatch-grid color picker (WT-style quick coloring), opened from "Choose Color…".
    TabColorFlyout {
        id: colorFlyout
        controller: menu.controller
        tabIndex: menu.tabIndex
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
