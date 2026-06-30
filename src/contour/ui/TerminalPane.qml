// vim:syntax=qml
// A single terminal pane: one ContourTerminal display bound to a leaf's session, plus the
// per-pane chrome (background, scrollbar) and an active-pane focus border.
//
// This is instantiated once per leaf of the split tree (by PaneNode.qml). Clicking the pane makes
// it the active pane of its tab (emitted via `activated`).
//
// It replicates the per-session wiring that the single-pane Terminal.qml installs in its
// onSessionChanged: the permission-wall dialogs, bell, window alert, OSC-777 notifications, and the
// bidirectional scrollbar binding. Without this, terminals living in a split pane silently lost all
// of those (permission requests were never shown, the bell was mute, the window never raised
// urgency, notifications were dropped, and the scrollbar was inert).
import QtQuick
import QtQuick.Controls
import QtQuick.Window
import Contour.Terminal

ContourTerminal {
    id: pane

    // The TerminalSession this pane renders.
    // (Set by the caller: `session: node.session`.)

    // Whether this pane is the active pane of its tab (drives the focus border).
    property bool paneActive: false

    // Deferred multimedia loading: the bell Loader activates only after the background thread has
    // finished probing the audio drivers (mirrors Terminal.qml).
    property real _pendingBellVolume: -1

    // When this pane becomes the active pane (mouse click OR a keyboard FocusPane* action, which
    // flips node.active via the model's activePaneChanged), grab Qt keyboard focus so keystrokes go
    // to this terminal — not just the focus border. Without this, keyboard pane navigation moved the
    // highlight but left typing routed to the previously focused pane.
    onPaneActiveChanged: if (paneActive) forceActiveFocus()

    // Raised when the user clicks this pane, so the owner can make it active.
    signal activated()

    visible: true

    // Per-pane background.
    Rectangle {
        anchors.fill: parent
        color: pane.session ? pane.session.backgroundColor : "black"
        z: -1
    }

    // Vertical scrollbar for this pane.
    ScrollBar {
        id: vbar
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        visible: pane.session ? pane.session.isScrollbarVisible : false
        orientation: Qt.Vertical
        policy: visible ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        minimumSize: 0.1
        size: pane.session
            ? pane.session.pageLineCount / (pane.session.pageLineCount + pane.session.historyLineCount)
            : 1.0
    }

    // Bell sound, loaded once multimedia is ready (same deferral as Terminal.qml).
    Loader {
        id: bellLoader
        active: terminalSessions.multimediaReady
        source: "BellSound.qml"
        onLoaded: {
            item.source = Qt.binding(function() { return pane.session ? pane.session.bellSource : ""; });
            if (pane._pendingBellVolume >= 0) {
                item.play(pane._pendingBellVolume);
                pane._pendingBellVolume = -1;
            }
        }
    }

    // Permission-wall dialogs (per pane, mirroring Terminal.qml).
    RequestPermission {
        id: requestFontChangeDialog
        text: "The host application is requesting to change the display font."
        onYesToAllClicked: pane.session.applyPendingFontChange(true, true);
        onYesClicked: pane.session.applyPendingFontChange(true, false);
        onNoToAllClicked: pane.session.applyPendingFontChange(false, true);
        onNoClicked: pane.session.applyPendingFontChange(false, false);
        onRejected: if (pane.session !== null) pane.session.applyPendingFontChange(false, false);
    }

    RequestPermission {
        id: requestLargeFilePaste
        text: "The host application is going to paste large file, are you sure?"
        onYesToAllClicked: pane.session.applyPendingPaste(true, true);
        onYesClicked: pane.session.applyPendingPaste(true, false);
        onNoToAllClicked: pane.session.applyPendingPaste(false, true);
        onNoClicked: pane.session.applyPendingPaste(false, false);
        onRejected: if (pane.session !== null) pane.session.applyPendingPaste(false, false);
    }

    RequestPermission {
        id: requestBufferCaptureDialog
        text: "The host application is requesting to capture the terminal buffer."
        onYesToAllClicked: pane.session.executePendingBufferCapture(true, true);
        onYesClicked: pane.session.executePendingBufferCapture(true, false);
        onNoToAllClicked: pane.session.executePendingBufferCapture(false, true);
        onNoClicked: pane.session.executePendingBufferCapture(false, false);
        onRejected: pane.session.executePendingBufferCapture(false, false);
    }

    RequestPermission {
        id: requestShowHostWritableStatusLine
        text: "The host application is requesting to show the host-writable statusline."
        onYesToAllClicked: pane.session.executeShowHostWritableStatusLine(true, true);
        onYesClicked: pane.session.executeShowHostWritableStatusLine(true, false);
        onNoToAllClicked: pane.session.executeShowHostWritableStatusLine(false, true);
        onNoClicked: pane.session.executeShowHostWritableStatusLine(false, false);
        onRejected: pane.session.executeShowHostWritableStatusLine(false, false);
    }

    // Active-pane focus border.
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: pane.paneActive ? 1 : 0
        border.color: "#3daee9"
        z: 10
    }

    TapHandler {
        onTapped: pane.activated()
    }

    function playBell(volume) {
        if (bellLoader.status === Loader.Ready)
            bellLoader.item.play(volume);
        else
            _pendingBellVolume = volume;
    }

    function doAlert() {
        if (Window.window !== null)
            Window.window.alert(0);
    }

    // Update the VT's viewport whenever the scrollbar's position changes.
    function onScrollBarPositionChanged() {
        let vt = pane.session;
        if (vt === null)
            return;
        let totalLineCount = (vt.pageLineCount + vt.historyLineCount);
        if (vbar.active)
            vt.scrollOffset = vt.historyLineCount - vbar.position * totalLineCount;
    }

    // Update the GUI scrollbar whenever the VT's viewport changes.
    function updateScrollBarPosition() {
        let vt = pane.session;
        if (vt === null)
            return;
        let totalLineCount = (vt.pageLineCount + vt.historyLineCount);
        vbar.position = (vt.historyLineCount - vt.scrollOffset) / totalLineCount;
    }

    // Wire the per-session signals whenever the backing session changes. Mirrors Terminal.qml's
    // onSessionChanged so a split pane behaves like the single-pane view.
    onSessionChanged: (s) => {
        let vt = pane.session;
        if (vt === null)
            return;

        // Bell, window alert, and OSC-777 notifications.
        vt.onBell.connect(playBell);
        vt.onAlert.connect(doAlert);
        vt.onShowNotification.connect(pane.showNotification);

        // Bidirectional scrollbar binding (drag -> viewport, scroll -> thumb).
        vbar.onPositionChanged.connect(onScrollBarPositionChanged);
        vbar.onSizeChanged.connect(updateScrollBarPosition);
        vt.onScrollOffsetChanged.connect(updateScrollBarPosition);

        // Permission-wall hooks.
        vt.requestPermissionForFontChange.connect(requestFontChangeDialog.open);
        vt.requestPermissionForBufferCapture.connect(requestBufferCaptureDialog.open);
        vt.requestPermissionForShowHostWritableStatusLine.connect(requestShowHostWritableStatusLine.open);
        vt.requestPermissionForPasteLargeFile.connect(requestLargeFilePaste.open);
    }
}
