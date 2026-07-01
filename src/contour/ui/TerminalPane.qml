// vim:syntax=qml
// A single terminal pane: one ContourTerminal display bound to a leaf's session, plus the
// per-pane chrome (background, scrollbar) and an active-pane focus border.
//
// This is instantiated once per leaf of the split tree (by PaneNode.qml). Clicking the pane makes
// it the active pane of its tab (emitted via `activated`).
//
// The per-session wiring (permission-wall dialogs, bell, window alert, OSC-777 notifications, and the
// bidirectional scrollbar binding) is shared with the single-pane Terminal.qml via SessionChrome.qml;
// this file only adds the pane-specific background and active-pane focus border. Without SessionChrome,
// terminals in a split pane would silently lose all of those (permission requests never shown, mute
// bell, no urgency, dropped notifications, inert scrollbar).
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

    // Shared per-session chrome (scrollbar, bell, permission dialogs, notification/alert wiring).
    SessionChrome {
        id: chrome
        session: pane.session
        displayItem: pane
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

    // Wire the shared chrome to the backing session whenever it changes. Mirrors Terminal.qml so a
    // split pane behaves like the single-pane view.
    onSessionChanged: (s) => {
        chrome.wireSession(pane.session);
    }
}
