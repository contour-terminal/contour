// vim:syntax=qml
// A quiet chip, bottom-right of the window content, shown while the daemon this client talks to runs
// another release than the client. Clicking it offers to restart the daemon on the new version, after a
// confirmation that says what the restart costs.
//
// Placed by its user (Main.qml anchors it bottom-right of the window content).
//
// It reads the `daemonDrift` context property (window::DaemonDriftModel) and neither decides what drifts
// nor performs the restart: it shows the model's line and forwards the user's confirmation.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: root
    objectName: "daemonDriftBadge"

    // Guarded with typeof: a window hosted without a daemon (and the lightweight test harnesses) sets no
    // such context property, and an unresolved name in a binding is a ReferenceError.
    readonly property var model: typeof daemonDrift !== "undefined" ? daemonDrift : null

    visible: root.model !== null && root.model.isVisible

    implicitWidth: label.implicitWidth + 16
    implicitHeight: label.implicitHeight + 8
    radius: 4
    color: Qt.rgba(sys.window.r, sys.window.g, sys.window.b, 0.85)
    border.color: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.25)
    border.width: 1

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }

    Label {
        id: label
        objectName: "daemonDriftLabel"
        anchors.centerIn: parent
        text: root.model !== null ? root.model.text : ""
        font: chromeStyle.captionFont
        color: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.7)
    }

    MouseArea {
        id: area
        objectName: "daemonDriftArea"
        anchors.fill: parent
        enabled: root.model !== null && !root.model.isRestarting
        cursorShape: Qt.PointingHandCursor
        onClicked: confirmDialog.open()
    }

    ConfirmDialog {
        id: confirmDialog
        objectName: "daemonDriftConfirm"
        heading: root.model !== null ? root.model.actionLabel : ""
        message: root.model !== null ? root.model.confirmation : ""
        confirmText: qsTr("Restart daemon")
        onConfirmed: root.model.requestRestart()
    }
}
