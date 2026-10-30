// vim:syntax=qml
// A frameless window's title-bar gestures over the item this fills: a drag moves the window, a double-click
// maximises or restores it, as on the terminal window's title bar (TitleBar.qml). Declared before the row's
// own content, so a control on the row keeps its clicks and only the row's empty places take the gestures.
import QtQuick
import QtQuick.Window

Item {
    id: root

    // What the gestures ask of the window: toggleMaximized(). Null turns them off.
    property var windowController: null

    TapHandler {
        objectName: "titleBarGesturesTap"
        enabled: root.windowController !== null
        onDoubleTapped: root.windowController.toggleMaximized()
    }
    DragHandler {
        objectName: "titleBarGesturesDrag"
        enabled: root.windowController !== null
        target: null
        acceptedButtons: Qt.LeftButton
        // Not CanTakeOverFromItems: the resize border's top edge covers the row's top, as it does the terminal
        // window's title bar (TabItem.qml, issue #1997).
        grabPermissions: PointerHandler.CanTakeOverFromHandlersOfDifferentType
                         | PointerHandler.ApprovesTakeOverByHandlersOfDifferentType
                         | PointerHandler.ApprovesTakeOverByItems
                         | PointerHandler.ApprovesCancellation
        onActiveChanged: if (active) root.Window.window.startSystemMove()
    }
}
