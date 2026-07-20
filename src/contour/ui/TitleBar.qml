// vim:syntax=qml
// The custom (client-side-decoration) title bar: tab strip + draggable region + window controls.
//
// Because the window is frameless, this bar owns everything the native decoration used to: the tab
// strip on the left, an empty draggable region in the middle (drag to move the window, double-click
// to maximize/restore), and the min/max/close controls on the right.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: root

    Accessible.role: Accessible.TitleBar
    Accessible.name: qsTr("Title bar")

    required property var controller // this window's WindowController (main.qml: appWindow.win)
    required property var window

    // Whether to draw our own min/max/close controls. Set false whenever the NATIVE frame is shown
    // (its server-side controls would otherwise be duplicated): that is when show_title_bar is on, on
    // any OS, and always on macOS where the native frame is kept. The owner (main.qml) drives this from
    // vtui.titleBarVisible; the default keeps the historical macOS behavior when unset.
    property bool useCustomWindowControls: Qt.platform.os !== "osx"

    implicitHeight: 34

    // Live OS palette handle: re-emits paletteChanged on an OS dark/light switch so the bar recolors
    // in realtime (see TabContextMenu for the same pattern).
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Opaque background so the transparent window doesn't show the desktop through the bar. The
    // ApplicationWindow itself is transparent for terminal see-through, so the bar must paint a fully
    // opaque fill of its own — sourced from the OS window color so it matches the system theme (the
    // palette roles are opaque QColors).
    Rectangle {
        anchors.fill: parent
        color: systemPalette.window
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        TabStrip {
            id: tabStrip
            Layout.fillHeight: true
            // Take only as much width as the tabs need, but never crowd out the drag region: the
            // RowLayout gives the remaining width to the fillWidth drag region below.
            Layout.preferredWidth: Math.min(implicitWidth, root.width * 0.7)
            controller: root.controller
            // Threaded down so a closed inline rename can hand keyboard focus back to the terminal.
            window: root.window
        }

        // Draggable empty region: move the window, double-click to toggle maximize, right-click for the
        // window's own context menu.
        Item {
            id: dragRegion
            objectName: "dragRegion"
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Right-click opens the title bar menu. It does not fight the handlers below: both of those
            // take the LEFT button only, which the DragHandler now says explicitly rather than leaving
            // to a default that a future edit could quietly change.
            TapHandler {
                acceptedButtons: Qt.RightButton
                // ReleaseWithinBounds rather than the default DragThreshold: a little jitter while the
                // button is down should still open the menu, not swallow the click.
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: if (root.controller) root.controller.openTitleBarContextMenu()
            }

            TapHandler {
                // Routed through the controller (the only window-geometry mutator): direct
                // QWindow show-mode calls skip its size-increment protocol.
                onTapped: (eventPoint, button) => {
                    if (tapCount === 2)
                        root.controller.toggleMaximized()
                }
            }
            DragHandler {
                target: null
                acceptedButtons: Qt.LeftButton
                onActiveChanged: if (active) root.window.startSystemMove()
            }
        }

        WindowControls {
            id: controls
            Layout.fillHeight: true
            visible: root.useCustomWindowControls
            window: root.window
            controller: root.controller
        }
    }
}
