// vim:syntax=qml
// The custom (client-side-decoration) title bar: tab strip + draggable region + window controls.
//
// Because the window is frameless, this bar owns everything the native decoration used to: the tab
// strip, an empty draggable region (drag to move the window, double-click to maximize/restore), and
// the min/max/close controls. Which END of the bar those controls sit at is the configured
// window_control_style's to decide (`windowControls.side`), which is why there is a slot for them
// at each edge and never an anchor pinning them to one.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: root

    Accessible.role: Accessible.TitleBar
    Accessible.name: qsTr("Title bar")

    required property var controller // this window's WindowController (Main.qml: appWindow.win)
    required property var window

    // Whether to draw our own min/max/close controls. Set false whenever the NATIVE frame is shown,
    // on any OS, because its server-side controls would otherwise be duplicated -- that is exactly
    // show_title_bar, and the owner (Main.qml) drives this from the controller's titleBarVisible.
    //
    // Not a platform test: it was one (`Qt.platform.os !== "osx"`), from when macOS kept its native
    // frame unconditionally, but Main.qml has overridden it unconditionally ever since, so the check
    // decided nothing. What macOS actually wants is macOS-shaped controls, which is
    // window_control_style's job, not this flag's.
    property bool useCustomWindowControls: true

    // The whole bar's height, and so the window's chrome height (Main.qml declares it to the
    // WindowController, which feeds the WM size hints). One character row in the terminal style,
    // the historical 34px in the native one -- see UiStyleProvider.
    implicitHeight: chromeStyle.chromeHeight

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

    // The window controls, wherever they go. One definition, instantiated by whichever of the two
    // Loaders below the configured side turns on; the other stays inactive and so has no implicit
    // size at all, which is what lets the layout collapse it rather than reserve width for it.
    Component {
        id: windowControlsComponent

        WindowControls {
            window: root.window
            controller: root.controller
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Leading slot: macOS puts its traffic lights here, before the tabs.
        Loader {
            objectName: "leadingWindowControls"
            Layout.fillHeight: true
            active: root.useCustomWindowControls && windowControls.side === "leading"
            sourceComponent: windowControlsComponent
        }

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
                objectName: "titleBarDragHandler" // findChild() handle for the GUI test
                target: null
                acceptedButtons: Qt.LeftButton
                // The default grabPermissions minus CanTakeOverFromItems, for the same reason as the tab
                // drag handler in TabItem (issue #1997): the resize border's top edge zone covers the top
                // six pixels of this region, and taking its grab asked for a window MOVE on top of the
                // resize already under way. See TabItem.qml for why the rule belongs on the handler.
                grabPermissions: PointerHandler.CanTakeOverFromHandlersOfDifferentType
                                 | PointerHandler.ApprovesTakeOverByHandlersOfDifferentType
                                 | PointerHandler.ApprovesTakeOverByItems
                                 | PointerHandler.ApprovesCancellation
                onActiveChanged: if (active) root.window.startSystemMove()
            }
        }

        // Trailing slot: where Windows and Breeze put theirs, after the drag region.
        Loader {
            objectName: "trailingWindowControls"
            Layout.fillHeight: true
            active: root.useCustomWindowControls && windowControls.side === "trailing"
            sourceComponent: windowControlsComponent
        }
    }
}
