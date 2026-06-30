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
        acceptedButtons: Qt.LeftButton
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
