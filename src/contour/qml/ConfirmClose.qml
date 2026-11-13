// vim:syntax=qml
// The one confirmation for closing a workspace, a tab or a pane. The sidebar's menu always asks
// (WindowController.sidebarCloseConfirmationRequested), and a workspace whose panes run programs asks when it is
// closed any other way (closeWorkspaceConfirmationRequested). What it says comes from the window's table, one row
// per kind (WindowController.closeConfirmation), so this item has no per-kind branch: Cancel is the default and
// Escape cancels; the destructive button calls WindowController.confirmClose with the node's ids.
import QtQuick
import Contour.Terminal

KitDialog {
    id: root
    objectName: "closeConfirmation"

    /// The window's controller.
    property var controller: null
    /// The node's kind, a SidebarRows.CloseKind.
    property int kind: 0
    /// The ids naming the node; 64-bit, so not ints. A workspace has no tab and no pane.
    property var workspace: 0
    property var tab: 0
    property var pane: 0
    /// The node's name; empty when it has none.
    property string nodeLabel
    /// The running program's name; empty when none is known.
    property string program

    /// What the table says for the node: { heading, message, acceptText }.
    readonly property var wording: root.controller
        ? root.controller.closeConfirmation(root.kind, root.workspace, root.tab, root.nodeLabel, root.program)
        : ({})

    /// Asks about the node the arguments name.
    function ask(nodeKind, workspaceId, tabId, paneId, label, programName) {
        root.kind = nodeKind
        root.workspace = workspaceId
        root.tab = tabId
        root.pane = paneId
        root.nodeLabel = label
        root.program = programName
        root.open()
    }

    heading: root.wording.heading ?? ""
    message: root.wording.message ?? ""
    acceptText: root.wording.acceptText ?? qsTr("Close")
    rejectText: qsTr("Cancel")
    acceptRole: chromeStyle.statusDanger
    defaultButton: KitDialog.Reject
    onAccepted: if (root.controller) root.controller.confirmClose(root.kind, root.workspace, root.tab, root.pane)
}
