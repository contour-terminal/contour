// vim:syntax=qml
// Shared per-session chrome for a ContourTerminal display.
//
// Both the single-pane view (Terminal.qml) and each split-tree leaf (TerminalPane.qml) need the same
// per-session widgets and wiring: the vertical scrollbar (bidirectionally bound to the VT viewport),
// the deferred bell sound, the four permission-wall dialogs, the window alert, and the OSC-777
// notification relay. This component owns all of that so neither host has to hand-maintain a second
// copy — previously TerminalPane.qml duplicated ~120 lines of Terminal.qml's chrome, and the two
// drifted (e.g. the pane hardcoded a right-side scrollbar and dropped stepSize).
//
// The host embeds this as a child and forwards its own session pointer:
//
//     SessionChrome { id: chrome; session: <root>.session; displayItem: <root> }
//
// and calls chrome.wireSession(vt) from its own onSessionChanged, because the connect targets
// (the scrollbar, the dialog .open slots) live inside this component. The host keeps whatever extra,
// non-shared wiring it has (Terminal.qml's font/opacity/size relays; TerminalPane.qml's focus border).
//
// Every session access is null-guarded: a split pane's session can be rebound to null during teardown,
// and the single-pane session is never null, so the guards are safe for both.
// SessionChrome is a plain Item (not a ContourTerminal), so it needs no Contour.Terminal import; its
// sibling components (RequestPermission, BellSound) resolve via the implicit same-directory import.
import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: chrome
    anchors.fill: parent

    // The TerminalSession this chrome is bound to (forwarded by the host; may be null transiently).
    required property var session

    // The enclosing ContourTerminal display, used to relay showNotification and to grab keyboard focus.
    property var displayItem: parent

    // Deferred multimedia loading: the bell Loader activates only after the background thread has
    // finished probing the audio/codec drivers. Holds a pending volume if a bell arrives before the
    // Loader is ready.
    property real _pendingBellVolume: -1

    // Vertical scrollbar, bidirectionally bound to the VT viewport (see wireSession()). Honors the
    // session's left/right placement preference; all reads are null-guarded.
    ScrollBar {
        id: vbar
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: (chrome.session && chrome.session.isScrollbarRight) ? parent.right : undefined
        anchors.left: (chrome.session && chrome.session.isScrollbarRight) ? undefined : parent.left
        visible: chrome.session ? chrome.session.isScrollbarVisible : false
        orientation: Qt.Vertical
        policy: visible ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        minimumSize: 0.1
        size: chrome.session
            ? chrome.session.pageLineCount / (chrome.session.pageLineCount + chrome.session.historyLineCount)
            : 1.0
        stepSize: chrome.session
            ? 1.0 / (chrome.session.pageLineCount + chrome.session.historyLineCount)
            : 0.0
    }

    // Bell sound, loaded once multimedia is ready.
    Loader {
        id: bellLoader
        active: terminalSessions.multimediaReady
        source: "BellSound.qml"
        onLoaded: {
            item.source = Qt.binding(function() { return chrome.session ? chrome.session.bellSource : ""; });
            if (chrome._pendingBellVolume >= 0) {
                item.play(chrome._pendingBellVolume);
                chrome._pendingBellVolume = -1;
            }
        }
    }

    // Permission-wall dialogs.
    RequestPermission {
        id: requestFontChangeDialog
        text: "The host application is requesting to change the display font."
        onYesToAllClicked: chrome.session.applyPendingFontChange(true, true);
        onYesClicked: chrome.session.applyPendingFontChange(true, false);
        onNoToAllClicked: chrome.session.applyPendingFontChange(false, true);
        onNoClicked: chrome.session.applyPendingFontChange(false, false);
        onRejected: if (chrome.session !== null) chrome.session.applyPendingFontChange(false, false);
    }

    RequestPermission {
        id: requestLargeFilePaste
        text: "The host application is going to paste large file, are you sure?"
        onYesToAllClicked: chrome.session.applyPendingPaste(true, true);
        onYesClicked: chrome.session.applyPendingPaste(true, false);
        onNoToAllClicked: chrome.session.applyPendingPaste(false, true);
        onNoClicked: chrome.session.applyPendingPaste(false, false);
        onRejected: if (chrome.session !== null) chrome.session.applyPendingPaste(false, false);
    }

    RequestPermission {
        id: requestBufferCaptureDialog
        text: "The host application is requesting to capture the terminal buffer."
        onYesToAllClicked: chrome.session.executePendingBufferCapture(true, true);
        onYesClicked: chrome.session.executePendingBufferCapture(true, false);
        onNoToAllClicked: chrome.session.executePendingBufferCapture(false, true);
        onNoClicked: chrome.session.executePendingBufferCapture(false, false);
        onRejected: if (chrome.session !== null) chrome.session.executePendingBufferCapture(false, false);
    }

    RequestPermission {
        id: requestShowHostWritableStatusLine
        text: "The host application is requesting to show the host-writable statusline."
        onYesToAllClicked: chrome.session.executeShowHostWritableStatusLine(true, true);
        onYesClicked: chrome.session.executeShowHostWritableStatusLine(true, false);
        onNoToAllClicked: chrome.session.executeShowHostWritableStatusLine(false, true);
        onNoClicked: chrome.session.executeShowHostWritableStatusLine(false, false);
        onRejected: if (chrome.session !== null) chrome.session.executeShowHostWritableStatusLine(false, false);
    }

    // Play a bell, deferring the volume if the sound Loader is not ready yet.
    function playBell(volume) {
        if (bellLoader.status === Loader.Ready)
            bellLoader.item.play(volume);
        else
            chrome._pendingBellVolume = volume;
    }

    // Flash the window's task-bar entry / urgency hint.
    function doAlert() {
        if (Window.window !== null)
            Window.window.alert(0);
    }

    // Relay an OSC-777 notification to the window's tray/notification logic. Routed through Window.window
    // (the ApplicationWindow's showNotification) rather than the display item: a ContourTerminal's
    // showNotification is a C++ signal, so connecting a session's notification to it for a split pane would
    // dead-end (signal->signal) and never reach the tray. Window-level relay works for every host uniformly.
    function relayNotification(title, body) {
        if (Window.window !== null && Window.window.showNotification !== undefined)
            Window.window.showNotification(title, body);
    }

    // Update the VT's viewport whenever the scrollbar's position changes.
    function onScrollBarPositionChanged() {
        let vt = chrome.session;
        if (vt === null)
            return;
        let totalLineCount = (vt.pageLineCount + vt.historyLineCount);
        if (vbar.active)
            vt.scrollOffset = vt.historyLineCount - vbar.position * totalLineCount;
    }

    // Update the GUI scrollbar whenever the VT's viewport changes.
    function updateScrollBarPosition() {
        let vt = chrome.session;
        if (vt === null)
            return;
        let totalLineCount = (vt.pageLineCount + vt.historyLineCount);
        vbar.position = (vt.historyLineCount - vt.scrollOffset) / totalLineCount;
    }

    // Wire the per-session signals to this chrome's widgets. The host calls this from its own
    // onSessionChanged (the connect targets — vbar, the dialogs — live here, not on the host). No-op
    // when the session is null, so a pane rebound to a null session during teardown is safe.
    function wireSession(vt) {
        if (vt === null)
            return;

        // Bell, window alert, and OSC-777 notifications (relayed to the window's tray logic).
        vt.onBell.connect(playBell);
        vt.onAlert.connect(doAlert);
        vt.onShowNotification.connect(relayNotification);

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
