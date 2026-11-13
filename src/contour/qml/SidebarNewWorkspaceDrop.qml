// vim:syntax=qml
// A place a pane or a tab is dropped on to get a workspace of its own (SidebarModel.dropOnNewWorkspace): the
// sidebar tree's empty area below its last row, and every New Workspace control. It is highlighted while a
// drag it takes hovers it; one it does not take (a workspace, or the only tab of one) is refused, and falls
// through to the window, which takes it as a no-op.
import QtQuick
import Contour.Terminal

DropArea {
    id: root

    /// The window's sidebar model (SidebarModel), which decides what is taken and carries the drop out.
    required property var sidebar
    /// Whether the drag over it now is one it takes.
    property bool takes: false
    /// The corner radius of the highlight.
    property real radius: chromeStyle.sidebar.selectionRadius

    keys: [NodeDrag.mime]

    function refresh(drag) {
        root.takes = !!root.sidebar && root.sidebar.takesNewWorkspaceDrop(drag.getDataAsString(NodeDrag.mime))
        drag.accepted = root.takes
    }

    onEntered: (drag) => root.refresh(drag)
    onPositionChanged: (drag) => root.refresh(drag)
    onExited: root.takes = false
    onDropped: (drop) => {
        const takes = root.takes
        root.takes = false
        if (!takes) {
            drop.accepted = false
            return
        }
        root.sidebar.dropOnNewWorkspace(drop.getDataAsString(NodeDrag.mime))
        drop.accept(Qt.MoveAction)
    }

    Rectangle {
        objectName: "sidebarNewWorkspaceDropHighlight"
        anchors.fill: parent
        visible: root.takes && root.containsDrag
        radius: root.radius
        readonly property color mark: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.accentColor
        color: Qt.alpha(mark, chromeStyle.dropFillOpacity)
        border.width: chromeStyle.borderWidth
        border.color: mark
    }
}
