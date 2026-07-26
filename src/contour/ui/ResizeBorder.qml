// vim:syntax=qml
// Edge and corner resize handles for a frameless window.
//
// A frameless window has no WM-drawn border, so we draw invisible hit zones around the window edges
// and corners that call QWindow::startSystemResize() with the appropriate edges. This restores
// native interactive resizing on X11, Wayland and Windows.
import QtQuick
import QtQuick.Window

Item {
    id: root

    required property var window
    property int thickness: 6

    anchors.fill: parent
    z: 1000

    // Helper to create one resize zone.
    component ResizeZone: MouseArea {
        required property int edges
        objectName: "resizeZone" // findChildren() handle for the GUI test; all eight share it
        acceptedButtons: Qt.LeftButton
        // States that this gesture is not up for grabs: it belongs to the window manager from the
        // startSystemResize() below onwards. These zones lie OVER the window chrome — the top-left corner
        // one sits exactly on the first tab, and the top edge covers the top of every tab and of the title
        // bar's window-move region — and a DragHandler down there takes a PASSIVE grab on press, so it sees
        // the moves whoever is on top. Taking the grab from here turned one press into a resize AND a tab
        // drag, or a resize AND a startSystemMove() (issue #1997).
        //
        // This is the INTENT, not the guarantee: preventStealing sets keepMouseGrab, which
        // QQuickPointerHandler::approveGrabTransition consults, but Qt honours it inconsistently — it holds
        // on Qt 6.4 and 6.11 and does NOT on 6.10. The enforcement therefore lives on the handlers we own,
        // which drop CanTakeOverFromItems from their grabPermissions; see TabItem.qml. Keep both: a handler
        // added beneath these zones later will not necessarily remember that rule.
        preventStealing: true
        onPressed: root.window.startSystemResize(edges)
    }

    // Edges
    ResizeZone {
        edges: Qt.LeftEdge
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: root.thickness
        cursorShape: Qt.SizeHorCursor
    }
    ResizeZone {
        edges: Qt.RightEdge
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: root.thickness
        cursorShape: Qt.SizeHorCursor
    }
    ResizeZone {
        edges: Qt.TopEdge
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: root.thickness
        cursorShape: Qt.SizeVerCursor
    }
    ResizeZone {
        edges: Qt.BottomEdge
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: root.thickness
        cursorShape: Qt.SizeVerCursor
    }

    // Corners (take priority via higher z).
    ResizeZone {
        edges: Qt.TopEdge | Qt.LeftEdge
        anchors { top: parent.top; left: parent.left }
        width: root.thickness; height: root.thickness
        cursorShape: Qt.SizeFDiagCursor
        z: 1
    }
    ResizeZone {
        edges: Qt.TopEdge | Qt.RightEdge
        anchors { top: parent.top; right: parent.right }
        width: root.thickness; height: root.thickness
        cursorShape: Qt.SizeBDiagCursor
        z: 1
    }
    ResizeZone {
        edges: Qt.BottomEdge | Qt.LeftEdge
        anchors { bottom: parent.bottom; left: parent.left }
        width: root.thickness; height: root.thickness
        cursorShape: Qt.SizeBDiagCursor
        z: 1
    }
    ResizeZone {
        edges: Qt.BottomEdge | Qt.RightEdge
        anchors { bottom: parent.bottom; right: parent.right }
        width: root.thickness; height: root.thickness
        cursorShape: Qt.SizeFDiagCursor
        z: 1
    }
}
