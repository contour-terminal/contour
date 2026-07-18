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
import Qt5Compat.GraphicalEffects
import Contour.Terminal

ContourTerminal {
    id: pane

    // Puts this item into the ACCESSIBILITY TREE. Without an Accessible attached property QtQuick does
    // not consider a plain QQuickItem accessible, so it never appears as a child of the window's
    // interface -- and an assistive client walking the tree from the desktop root can never reach it,
    // however complete the C++ QAccessibleInterface behind it is. (Querying the item directly still
    // worked, which is exactly what made this easy to miss.)
    //
    // The role here only gets the item ADMITTED to the tree; the interface an AT actually talks to is
    // the one contour::display::TerminalAccessible's factory hands out, which also carries the text
    // interface a magnifier reads to follow the caret.
    Accessible.role: Accessible.Terminal
    Accessible.name: qsTr("Terminal")
    Accessible.focusable: true

    // The TerminalSession this pane renders.
    // (Set by the caller: `session: node.session`.)

    // Whether this pane is the active pane of its tab (drives the focus border).
    property bool paneActive: false

    // When this pane becomes the active pane (mouse click OR a keyboard FocusPane* action, which
    // flips node.active via the model's activePaneChanged), grab Qt keyboard focus so keystrokes go
    // to this terminal — not just the focus border. Without this, keyboard pane navigation moved the
    // highlight but left typing routed to the previously focused pane.
    onPaneActiveChanged: if (paneActive) forceActiveFocus()

    // A split creates the NEW pane already active (paneActive bound to node.active, which the model sets
    // true on split), so onPaneActiveChanged never fires for the initial value and the fresh pane would
    // otherwise render focused-looking but never grab Qt keyboard focus — keystrokes go nowhere. Grab focus
    // on completion when created active, covering the split-creation case that onPaneActiveChanged misses.
    Component.onCompleted: if (paneActive) forceActiveFocus()

    // Raised when the user clicks this pane, so the owner can make it active.
    signal activated()

    // Lifecycle traces below are diagnostics, not user-facing events: they fire on the perfectly
    // normal "the shell exited" path. Route them through a category that is off by default (Warning
    // floor) so they stay silent in normal use but can be re-enabled for debugging via
    // QT_LOGGING_RULES="contour.pane.debug=true".
    LoggingCategory {
        id: paneLog
        name: "contour.pane"
        defaultLogLevel: LoggingCategory.Warning
    }

    visible: true

    // Item opacity follows the session's (profile) opacity, matching the single-pane view. Null-guarded
    // because the session is briefly null during a split/collapse rebind.
    opacity: pane.session ? pane.session.opacity : 1.0

    // Close the window when this pane's shell terminates AND it was the last session in the window.
    // canCloseWindow() returns false while other panes/tabs remain, so closing a non-last pane collapses
    // the split (driven by paneClosed -> rebuildActiveTabPaneProxies) without closing the window. This is
    // the sole window-close path now that the pane tree is the only renderer.
    onTerminated: {
        console.debug(paneLog, "Client process terminated in pane.");
        // Ask THIS OS window's controller (main.qml's `win`) whether its last pane just exited. Per-window,
        // so the last pane of this window closes only this window; other in-process windows are unaffected.
        if (Window.window && Window.window.win && Window.window.win.canCloseWindow())
            Window.window.close();
    }

    // Per-pane background: the solid profile color plus the profile's background_image (with its
    // configured opacity and optional blur) — ported from the deleted single-pane Terminal.qml so the
    // feature survives the pane tree. The layering mirrors master: base color (dimmed to
    // 1 - opacityBackground under a blurred image), the image at opacityBackground, the blur pass, and
    // the color wash that restores text contrast over a blurred image. All session reads are
    // null-guarded (the session is transiently null during split rebinds).
    Item {
        anchors.fill: parent
        z: -1

        Rectangle {
            anchors.fill: parent
            color: pane.session ? pane.session.backgroundColor : "black"
            opacity: (pane.session && pane.session.isImageBackground && pane.session.isBlurBackground)
                ? 1.0 - pane.session.opacityBackground
                : 1.0
        }

        Image {
            id: backgroundImage
            anchors.fill: parent
            opacity: pane.session ? pane.session.opacityBackground : 1.0
            focus: false
            visible: pane.session ? pane.session.isImageBackground : false
            source: pane.session ? pane.session.pathToBackground : ""
        }

        FastBlur {
            visible: pane.session ? pane.session.isBlurBackground : false
            anchors.fill: backgroundImage
            source: backgroundImage
            radius: 32
        }

        Rectangle {
            anchors.fill: parent
            color: pane.session ? pane.session.backgroundColor : "black"
            opacity: (pane.session && pane.session.isImageBackground && pane.session.isBlurBackground)
                ? 1.0 - pane.session.opacityBackground
                : 0.0
        }
    }

    // Cell-dimensions popup shown briefly on resize (mirrors the former single-pane Terminal.qml popup).
    // It is inherently per-pane: each pane resizes independently, driven by its own session's
    // line/column-count signals wired in onSessionChanged below.
    Rectangle {
        id: sizeWidget
        anchors.centerIn: parent
        border.width: 1
        border.color: "black"
        property int margin: 10
        color: "white"
        visible: false
        focus: false
        z: 20
        Timer {
            id: sizeWidgetTimer
            interval: 1000
            running: false
            onTriggered: sizeWidget.visible = false
        }
        Text {
            id: sizeWidgetText
            anchors.centerIn: parent
            text: pane.session
                ? "Size: " + pane.session.pageColumnsCount.toString() + " x " + pane.session.pageLineCount.toString()
                : ""
        }
    }

    function updateSizeWidget() {
        if (!pane.session)
            return;
        if (pane.session.upTime > 1.0 && pane.session.showResizeIndicator) {
            sizeWidgetText.text = "Size: " + pane.session.pageColumnsCount.toString()
                + " x " + pane.session.pageLineCount.toString();
            sizeWidget.width = sizeWidgetText.contentWidth + sizeWidget.margin;
            sizeWidget.height = sizeWidgetText.contentHeight;
            sizeWidget.visible = true;
            sizeWidgetTimer.running = true;
        }
    }

    // Shared per-session chrome (scrollbar, bell, permission dialogs, notification/alert wiring).
    // The chrome wires itself to `session` declaratively and re-targets on every rebind — no
    // imperative wiring call needed (or wanted: it would accumulate stale connections; see the
    // Connections rationale below).
    SessionChrome {
        id: chrome
        session: pane.session
        displayItem: pane
    }

    // Policy for the unfocused-dim overlay, kept a pure function (like PaneNode.clampRatio) so every
    // dim/pane-focus/window-focus combination is unit-testable offscreen, where window activation
    // cannot be synthesized.
    function dimOverlayColor(dim, isPaneActive, isWindowActive, base) {
        if (!(dim > 0.0) || (isPaneActive && isWindowActive))
            return "transparent"
        return Qt.alpha(base, dim)
    }

    // Unfocused-pane dimming (dim_unfocused profile option, default off): blends the pane toward its
    // own background color while it is not the focused pane — an inactive pane of a split, or any
    // pane of an unfocused window. Composited above the terminal content but below the focus border;
    // the renderer itself is untouched.
    Rectangle {
        objectName: "dimOverlay"
        anchors.fill: parent
        z: 5
        color: pane.dimOverlayColor(pane.session ? pane.session.dimUnfocused : 0.0,
                                    pane.paneActive,
                                    Window.active,
                                    pane.session ? pane.session.backgroundColor : "#000000")
        visible: color.a > 0
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

    // Show the cell-dimensions popup on page-size changes (resize). Use a Connections object rather than
    // imperative session.lineCountChanged.connect(updateSizeWidget) in onSessionChanged: the imperative
    // form re-connects on every session rebind (a split rebinds this pane's session repeatedly) WITHOUT
    // disconnecting the old one, so stale handlers accumulate and fire on a torn-down pane during teardown
    // — where even `pane` (the root id) resolves to null, throwing "Cannot read property 'session' of null".
    // Connections re-targets atomically and is torn down with the pane, so no stale handler survives.
    Connections {
        target: pane.session
        ignoreUnknownSignals: true
        function onLineCountChanged() { pane.updateSizeWidget(); }
        function onColumnsCountChanged() { pane.updateSizeWidget(); }
    }
}
