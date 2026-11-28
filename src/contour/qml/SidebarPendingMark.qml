// vim:syntax=qml
// The mark a sidebar row's status slot shows while a jump to it, or into its daemon's section, is under way
// (SidebarRows.StatusSlot.Pending): one cell, the look's own (UiStyleProvider.sidebarDaemonRowFace), pulsing
// softly where motion is allowed and standing still under Reduce Motion (chromeStyle.motion).
import QtQuick

Text {
    id: root
    objectName: "sidebarPendingMark"

    /// Whether the jump is under way: the mark shows, and pulses where motion is allowed.
    property bool pending: false
    /// Whether the mark is pulsing now: for the tests.
    readonly property bool pulsing: pulse.running

    visible: root.pending
    // One cell on the Terminal row however wide the face that has the glyph draws it, as the grid counts it.
    width: chromeStyle.sidebar.look === chromeStyle.sidebar.cellTree ? chromeStyle.cellWidth
                                                                     : chromeStyle.snapToCells(implicitWidth)
    horizontalAlignment: Text.AlignHCenter
    textFormat: Text.PlainText
    text: chromeStyle.sidebarDaemonRowFace.pendingMark ?? ""
    font: chromeStyle.type.body
    Accessible.ignored: true

    SequentialAnimation on opacity {
        id: pulse
        running: root.pending && chromeStyle.motion.animates
        loops: Animation.Infinite
        // Back to whole once it stops mid-pulse: an animation attached with `on opacity` leaves it where it was.
        onRunningChanged: if (!running) root.opacity = 1.0
        NumberAnimation { from: 1.0; to: 0.35; duration: chromeStyle.motion.pulse; easing.type: Easing.InOutQuad }
        NumberAnimation { from: 0.35; to: 1.0; duration: chromeStyle.motion.pulse; easing.type: Easing.InOutQuad }
    }
}
