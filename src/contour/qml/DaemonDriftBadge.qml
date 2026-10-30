// vim:syntax=qml
// A toolbar chip in the title band, shown while the daemon this client talks to runs another release than
// the client. Clicking it offers to restart the daemon on the new version, after a confirmation that says
// what the restart costs.
//
// Placed by its user: Main.qml puts it in the band's trailing area, and at the content's corner while no
// band is shown, so a drift never goes unsaid.
//
// It reads the `daemonDrift` context property (window::DaemonDriftModel) and neither decides what drifts
// nor performs the restart: it shows the model's line and forwards the user's confirmation. On the
// Terminal row the chip is a run of cells bracketed by the chip glyph tokens.
import QtQuick
import QtQuick.Controls

Item {
    id: root
    objectName: "daemonDriftBadge"

    // Guarded with typeof: a window hosted without a daemon (and the lightweight test harnesses) sets no
    // such context property, and an unresolved name in a binding is a ReferenceError.
    readonly property var model: typeof daemonDrift !== "undefined" ? daemonDrift : null
    // Whether a click may ask for the restart: not while one already runs.
    readonly property bool canRestart: root.model !== null && !root.model.isRestarting
    // Whether the chip is a run of bracketed cells (the Terminal row) rather than a rounded shape.
    readonly property bool bracketed: chromeStyle.chipOpenGlyph !== ""
    /// This window's WindowController, which a key the chip does not use is offered to; null for none.
    property var controller: null
    /// Whether keyboard focus is on the chip and the chrome says so: the host's to set.
    property bool focusRingShown: false

    Accessible.role: Accessible.Button
    Accessible.focusable: root.focusRingShown
    Accessible.focused: root.focusRingShown
    Accessible.name: label.text
    Accessible.onPressAction: root.offerRestart()

    visible: root.model !== null && root.model.isVisible

    implicitWidth: row.implicitWidth + (root.bracketed ? 0 : chromeStyle.space.toolbarChipPadding + chromeStyle.space.toolbarChipPadding)
    implicitHeight: chromeStyle.size.toolbarChip
    width: root.implicitWidth
    height: root.implicitHeight

    // Return and Space ask for the restart, as a click does; any other key is the chrome's (arrows, Escape, the
    // focus chords), whether the chip sits in the band or, with no band, at the content's corner.
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.offerRestart()
            event.accepted = true
        } else if (root.controller && root.controller.chromeKey) {
            event.accepted = root.controller.chromeKey(event.key, event.modifiers)
        }
    }

    /// Opens the confirmation, unless a restart already runs.
    function offerRestart() {
        if (root.canRestart)
            confirmDialog.open()
    }

    Rectangle {
        objectName: "daemonDriftBackground"
        anchors.fill: parent
        radius: chromeStyle.radii.toolbarChip
        color: chromeStyle.colors.chipFill
    }

    FocusRing {
        shown: root.focusRingShown
        hostRadius: chromeStyle.radii.toolbarChip
    }

    Row {
        id: row
        anchors.centerIn: parent

        Text {
            visible: root.bracketed
            text: chromeStyle.chipOpenGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.chipText
        }

        Label {
            id: label
            objectName: "daemonDriftLabel"
            text: root.model !== null ? root.model.text : ""
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.chipText
        }

        Text {
            visible: root.bracketed
            text: chromeStyle.chipCloseGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.chipText
        }
    }

    TapHandler {
        objectName: "daemonDriftArea"
        enabled: root.canRestart
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: root.offerRestart()
    }

    HoverHandler {
        enabled: root.canRestart
        cursorShape: Qt.PointingHandCursor
    }

    // Over the whole window, not centred on this chip: a dialog centres on its parent.
    KitDialog {
        id: confirmDialog
        objectName: "daemonDriftConfirm"
        parent: Overlay.overlay
        heading: root.model !== null ? root.model.actionLabel : ""
        message: root.model !== null ? root.model.confirmation : ""
        acceptText: qsTr("Restart daemon")
        rejectText: qsTr("Cancel")
        onAccepted: root.model.requestRestart()
    }
}
