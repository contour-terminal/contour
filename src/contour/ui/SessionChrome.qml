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
// The per-session wiring is declarative (the Connections element below): it re-targets — and
// auto-disconnects the previous session — on every `session` rebind, so a pane switched between
// sessions never accumulates stale cross-session connections (an imperative wireSession() that only
// ever connect()ed made a background tab's permission dialog answer on the WRONG session and played
// one bell N times after N tab switches). The host keeps whatever extra, non-shared wiring it has
// (TerminalPane.qml's focus border).
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

    // Vertical scrollbar, bidirectionally bound to the VT viewport (drag -> viewport here; viewport ->
    // thumb via the session Connections below). Honors the session's left/right placement preference;
    // all reads are null-guarded. The self-handlers are declared here (not connect()ed per session)
    // so they exist exactly once regardless of how often the session rebinds.
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
        onPositionChanged: chrome.onScrollBarPositionChanged()
        onSizeChanged: chrome.updateScrollBarPosition()
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
        onYesToAllClicked: if (chrome.session !== null) chrome.session.applyPendingFontChange(true, true);
        onYesClicked: if (chrome.session !== null) chrome.session.applyPendingFontChange(true, false);
        onNoToAllClicked: if (chrome.session !== null) chrome.session.applyPendingFontChange(false, true);
        onNoClicked: if (chrome.session !== null) chrome.session.applyPendingFontChange(false, false);
        onRejected: if (chrome.session !== null) chrome.session.applyPendingFontChange(false, false);
    }

    RequestPermission {
        id: requestLargeFilePaste
        text: "The host application is going to paste large file, are you sure?"
        onYesToAllClicked: if (chrome.session !== null) chrome.session.applyPendingPaste(true, true);
        onYesClicked: if (chrome.session !== null) chrome.session.applyPendingPaste(true, false);
        onNoToAllClicked: if (chrome.session !== null) chrome.session.applyPendingPaste(false, true);
        onNoClicked: if (chrome.session !== null) chrome.session.applyPendingPaste(false, false);
        onRejected: if (chrome.session !== null) chrome.session.applyPendingPaste(false, false);
    }

    RequestPermission {
        id: requestBufferCaptureDialog
        text: "The host application is requesting to capture the terminal buffer."
        onYesToAllClicked: if (chrome.session !== null) chrome.session.executePendingBufferCapture(true, true);
        onYesClicked: if (chrome.session !== null) chrome.session.executePendingBufferCapture(true, false);
        onNoToAllClicked: if (chrome.session !== null) chrome.session.executePendingBufferCapture(false, true);
        onNoClicked: if (chrome.session !== null) chrome.session.executePendingBufferCapture(false, false);
        onRejected: if (chrome.session !== null) chrome.session.executePendingBufferCapture(false, false);
    }

    RequestPermission {
        id: requestShowHostWritableStatusLine
        text: "The host application is requesting to show the host-writable statusline."
        onYesToAllClicked: if (chrome.session !== null) chrome.session.executeShowHostWritableStatusLine(true, true);
        onYesClicked: if (chrome.session !== null) chrome.session.executeShowHostWritableStatusLine(true, false);
        onNoToAllClicked: if (chrome.session !== null) chrome.session.executeShowHostWritableStatusLine(false, true);
        onNoClicked: if (chrome.session !== null) chrome.session.executeShowHostWritableStatusLine(false, false);
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

    // Wire the per-session signals to this chrome's widgets. Declarative on purpose: Connections
    // re-targets on every `session` rebind and disconnects the previous session automatically, so a
    // pane switched between sessions never keeps stale cross-session wiring. A null target (the
    // transient split-teardown state) simply connects nothing. ignoreUnknownSignals tolerates mock
    // sessions in the offscreen tests. NB: the doubled `onOn…` prefixes are correct — the C++ signals
    // are named onBell/onAlert.
    Connections {
        target: chrome.session
        ignoreUnknownSignals: true

        // Bell, window alert, and OSC-777 notifications (relayed to the window's tray logic).
        function onOnBell(volume) { chrome.playBell(volume); }
        function onOnAlert() { chrome.doAlert(); }
        function onShowNotification(title, body) { chrome.relayNotification(title, body); }

        // Viewport -> scrollbar thumb (the drag direction lives on the ScrollBar's own handlers).
        function onScrollOffsetChanged() { chrome.updateScrollBarPosition(); }

        // Permission-wall hooks.
        function onRequestPermissionForFontChange() { requestFontChangeDialog.open(); }
        function onRequestPermissionForBufferCapture() { requestBufferCaptureDialog.open(); }
        function onRequestPermissionForShowHostWritableStatusLine() { requestShowHostWritableStatusLine.open(); }
        function onRequestPermissionForPasteLargeFile() { requestLargeFilePaste.open(); }
    }
}
