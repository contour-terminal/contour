// vim:syntax=qml
// Right-click context menu for a tab: rename, color, and close operations.
import QtQuick
import QtQuick.Controls

Menu {
    id: menu

    required property var controller
    required property int tabIndex

    // Raised when the user picks "Rename"; the TabButton starts its inline rename field.
    signal renameRequested()

    // System palette handle: a live view of the OS QPalette that re-emits paletteChanged when the OS
    // dark/light theme flips, so every binding below recolors in realtime without restarting.
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Drive the menu and its items from the OS palette so item text/selection adapt to dark/light.
    // Without this, the Basic style's items keep their default (dark-only) colors even though the
    // background below is themed.
    palette {
        window: systemPalette.window
        windowText: systemPalette.windowText
        text: systemPalette.text
        highlight: systemPalette.highlight
        highlightedText: systemPalette.highlightedText
    }

    // Opaque, themed background. The ApplicationWindow is transparent (for terminal see-through) and
    // no Qt Quick Controls style is configured, so the default Basic Menu would render with an empty
    // background and the terminal would show straight through the popup. Painting an opaque OS-window
    // fill here is the fix. NB: do NOT set width/height — the Menu's content drives the size; the
    // background only contributes color/frame.
    background: Rectangle {
        color: systemPalette.window
        border.color: systemPalette.mid
        border.width: 1
        radius: 4
    }

    MenuItem {
        text: qsTr("Rename…")
        onTriggered: menu.renameRequested()
    }

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
