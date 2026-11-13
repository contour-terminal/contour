// vim:syntax=qml
// A frameless window's title-bar gestures over the item this fills: a drag moves the window, a double-click
// maximises or restores it, and a right-click opens the window's own menu where the window has one. The one
// implementation of them: the terminal window's band (TitleBar.qml), the header bar's part above the sidebar,
// the sidebar's title row and the settings window's header all fill their empty places with it. Declared
// before the row's own content, so a control on the row keeps its clicks and only the row's empty places
// take the gestures.
import QtQuick
import QtQuick.Window

Item {
    id: root

    // What the gestures ask of the window: toggleMaximized(), and openTitleBarContextMenu() where it has a
    // window menu. Null turns them off.
    property var windowController: null

    // Right-click opens the title bar menu. It does not fight the handlers below: both of those take the LEFT
    // button only, which the DragHandler says explicitly rather than leaving to a default that a future edit
    // could quietly change.
    TapHandler {
        objectName: "titleBarGesturesMenu"
        enabled: root.windowController !== null && !!root.windowController.openTitleBarContextMenu
        acceptedButtons: Qt.RightButton
        // ReleaseWithinBounds rather than the default DragThreshold: a little jitter while the button is down
        // should still open the menu, not swallow the click.
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: root.windowController.openTitleBarContextMenu()
    }
    TapHandler {
        objectName: "titleBarGesturesTap"
        enabled: root.windowController !== null
        // Routed through the controller (the only window-geometry mutator): direct QWindow show-mode calls
        // skip its size-increment protocol.
        onDoubleTapped: root.windowController.toggleMaximized()
    }
    DragHandler {
        objectName: "titleBarGesturesDrag"
        enabled: root.windowController !== null
        target: null
        acceptedButtons: Qt.LeftButton
        // The default grabPermissions minus CanTakeOverFromItems, for the same reason as the tab drag handler
        // in TabItem (issue #1997): the resize border's top edge zone covers the top of the row, and taking
        // its grab asked for a window MOVE on top of the resize already under way. See TabItem.qml for why the
        // rule belongs on the handler.
        grabPermissions: PointerHandler.CanTakeOverFromHandlersOfDifferentType
                         | PointerHandler.ApprovesTakeOverByHandlersOfDifferentType
                         | PointerHandler.ApprovesTakeOverByItems
                         | PointerHandler.ApprovesCancellation
        onActiveChanged: if (active) root.Window.window.startSystemMove()
    }
}
