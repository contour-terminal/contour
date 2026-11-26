// vim:syntax=qml
// The one confirmation for closing a workspace, a tab or a pane. The sidebar's menu always asks
// (WindowController.sidebarCloseConfirmationRequested), and a workspace whose panes run programs asks when it is
// closed any other way (closeWorkspaceConfirmationRequested). What it says comes from the window's table, one row
// per kind (WindowController.closeConfirmation), so this item has no per-kind branch: Cancel is the default and
// Escape cancels; the destructive button calls WindowController.confirmClose with the node it was asked about.
// The node -- the kind of close and its row, daemon and run included -- is an opaque value only the dialog holds
// while it asks: the window keeps nothing of the question, so an answer can only ever close what it was about.
import QtQuick
import Contour.Terminal

KitDialog {
    id: root
    objectName: "closeConfirmation"

    /// The window's controller.
    property var controller: null
    /// The node asked about (a WindowController CloseNode), opaque here; undefined while nothing is asked.
    property var node: undefined
    /// The node's name; empty when it has none.
    property string nodeLabel
    /// The running program's name; empty when none is known.
    property string program

    /// What the table says for the node, worded once as it is asked: { heading, message, acceptText }. Not a
    /// binding on the node, which the answer drops while the dialog fades out.
    property var wording: ({})

    /// Asks about @p closeNode, named @p label, running @p programName.
    function ask(closeNode, label, programName) {
        root.node = closeNode
        root.nodeLabel = label
        root.program = programName
        root.wording = root.controller ? root.controller.closeConfirmation(closeNode, label, programName) : ({})
        root.open()
    }

    heading: root.wording.heading ?? ""
    message: root.wording.message ?? ""
    acceptText: root.wording.acceptText ?? qsTr("Close")
    rejectText: qsTr("Cancel")
    acceptRole: chromeStyle.statusDanger
    defaultButton: KitDialog.Reject
    onAccepted: {
        const closing = root.node
        root.node = undefined
        if (root.controller && closing !== undefined)
            root.controller.confirmClose(closing)
    }
    // Cancel or Escape: the question is dropped with its node, and nothing closes.
    onRejected: root.node = undefined
}
