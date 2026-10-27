// vim:syntax=qml
// A single terminal pane: one ContourTerminal display bound to a leaf's session, plus the
// per-pane chrome (background, scrollbar) and the unfocused dim.
//
// This is instantiated once per leaf of the split tree (by PaneCard.qml, which draws the card and the
// header around it). Clicking the pane makes it the active pane of its tab (emitted via `activated`).
// Focus is told by the header's emphasis and the dim, never by an outline.
//
// The per-session wiring (permission-wall dialogs, bell, window alert, the session's notices, and the
// bidirectional scrollbar binding) is shared with the single-pane Terminal.qml via SessionChrome.qml;
// this file only adds the pane-specific background and the unfocused dim. Without SessionChrome,
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

    // Whether this pane is the active pane of its tab (drives the unfocused dim).
    property bool paneActive: false

    // The panes the tab shows (set by PaneCard): Minimal's dim floor applies only between two or more.
    property int visiblePanes: 1

    // The card's corner radius, which the pane's background and dim follow (0 outside a card).
    property real cornerRadius: 0

    // How far the rounded background reaches above the pane, clipped: the card's radius under an inline
    // header, whose own bottom edge is square, so the pane's top corners are square to meet it.
    property real squareTop: 0

    // The same below: the card's radius above an ended pane's footer, which the pane meets edge to edge.
    // The background and dim then stop square at the pane's bottom, reaching no backgroundOutset below it.
    property real squareBottom: 0

    // How far the pane's background and dim reach beyond it on every side: a rounded card keeps the grid
    // in from its corners (window::cardContentInset), and its colour fills the card up to the edge.
    property real backgroundOutset: 0

    // The pane header's find slot, and whether the pane has an inline header (a PaneHeaderPresence's value),
    // for the find bar (SessionChrome's SearchBar); and whether the bar sits in that header row now.
    property Item headerFindSlot: null
    property int headerPresent: 0
    // A bool by the QML-facing carve-out: it relays SearchBar.occupiesHeaderRow, a predicate derived from
    // the placement's named anchor, up to PaneCard's header; it decides nothing itself.
    readonly property bool findInHeaderRow: chrome.findInHeaderRow

    // When this pane becomes the active pane (mouse click OR a keyboard FocusPane* action, which
    // flips node.active via the model's activePaneChanged), grab Qt keyboard focus so keystrokes go
    // to this terminal — not just the focus cue. Without this, keyboard pane navigation moved the
    // cue but left typing routed to the previously focused pane.
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
        // Ask THIS OS window's controller (Main.qml's `win`) whether its last pane just exited. Per-window,
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
    //
    // The two colour layers fill the card (backgroundOutset beyond the grid) and take its corners
    // (cornerRadius, squared at the top by squareTop and at the bottom by squareBottom); the image and its
    // blur keep to the grid, which a card keeps clear of its corners.
    Item {
        objectName: "paneBackground"
        anchors.fill: parent
        anchors.margins: -pane.backgroundOutset
        anchors.bottomMargin: pane.squareBottom > 0 ? 0 : -pane.backgroundOutset
        z: -1
        clip: pane.squareTop > 0 || pane.squareBottom > 0

        Rectangle {
            anchors.fill: parent
            anchors.topMargin: -pane.squareTop
            anchors.bottomMargin: -pane.squareBottom
            radius: pane.cornerRadius
            color: pane.session ? pane.session.backgroundColor : "black"
            opacity: (pane.session && pane.session.isImageBackground && pane.session.isBlurBackground)
                ? 1.0 - pane.session.opacityBackground
                : 1.0
        }

        Image {
            id: backgroundImage
            anchors.fill: parent
            anchors.margins: pane.backgroundOutset
            anchors.bottomMargin: pane.squareBottom > 0 ? 0 : pane.backgroundOutset
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
            anchors.topMargin: -pane.squareTop
            anchors.bottomMargin: -pane.squareBottom
            radius: pane.cornerRadius
            color: pane.session ? pane.session.backgroundColor : "black"
            opacity: (pane.session && pane.session.isImageBackground && pane.session.isBlurBackground)
                ? 1.0 - pane.session.opacityBackground
                : 0.0
        }
    }

    // The size indicator: "100 × 30" after a resize, "13 pt" after a zoom, as a Toast pill centred on this
    // pane -- each pane of a split its own. The session decides when it is raised and what it says
    // (requestSizeIndicator); the pane holds it for motion.linger after the last request, then tells the
    // session it settled, which announces it for the active pane alone. Above the unfocused dim, so a resized
    // inactive pane still reads its size; a pane too narrow for the pill shows none.
    property bool sizeIndicatorHeld: false

    /// Where the pane's own overlays (the size indicator, the link pill) are drawn: an item its host lays
    /// exactly over this pane but outside any fade the host applies to it (PaneCard's inactive-card fade on the
    /// Terminal row), so the pills read at full strength. Null: the pane's own overlay layer, above its dim.
    property Item overlayHost: null
    /// The pane header's link pill slot, where the pane shows an inline header (PaneCard); null where none.
    property Item headerLinkSlot: null
    /// Whether the link pill sits in the pane header (QML-facing): the header hides what the pill covers.
    readonly property bool linkPillInHeader: chrome.linkPillInHeader

    // The pane's own overlay layer, above the unfocused dim, where its pills sit when no host gives them one.
    Item {
        id: paneOverlay
        objectName: "paneOverlay"
        anchors.fill: parent
        z: 20
    }

    Toast {
        id: sizeIndicator
        objectName: "sizeIndicator"
        parent: pane.overlayHost ? pane.overlayHost : paneOverlay
        anchors.centerIn: parent
        // Only once its row has laid out does the pill know its width; until then it does not fit anywhere.
        readonly property bool fits: implicitContentWidth > 0 && implicitWidth <= pane.width - 2 * chromeStyle.space.s4
        shown: pane.sizeIndicatorHeld && fits
        text: pane.session && pane.session.sizeIndicatorText !== undefined ? pane.session.sizeIndicatorText : ""
        // Tabular digits, so "99 × 30" and "100 × 30" keep their digits in place as a drag goes by.
        textFont: {
            var body = chromeStyle.type.body
            var spec = { family: body.family, weight: Font.DemiBold, features: { "tnum": 1 } }
            if (body.pixelSize > 0)
                spec.pixelSize = body.pixelSize
            else
                spec.pointSize = body.pointSize
            return Qt.font(spec)
        }
        // An in-pane pill casts no hard shadow on the Terminal row: only a real popover does.
        elevation: chromeStyle.elevation.inPane
        // Opaque on a translucent profile too (ruling A5): it sits outside the pane's opacity, and its
        // legibility comes before matching the pane's translucency.
        // The session announces the settled size itself, once.
        Accessible.ignored: true
    }

    // A pill held for the session that leaves would never settle for the one that comes: neither is shown.
    onSessionChanged: {
        pane.sizeIndicatorHeld = false
        sizeIndicatorHold.stop()
    }

    Timer {
        id: sizeIndicatorHold
        objectName: "sizeIndicatorHold"
        interval: chromeStyle.motion.linger
        onTriggered: {
            pane.sizeIndicatorHeld = false
            if (pane.session && pane.session.sizeIndicatorSettled !== undefined)
                pane.session.sizeIndicatorSettled(pane.paneActive)
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
        overlayHost: pane.overlayHost ? pane.overlayHost : paneOverlay
        headerFindSlot: pane.headerFindSlot
        headerLinkSlot: pane.headerLinkSlot
        headerPresent: pane.headerPresent
    }

    // Policy for the unfocused-dim overlay, kept a pure function (like PaneNode.clampRatio) so every
    // dim/pane-focus/window-focus combination is unit-testable offscreen, where window activation
    // cannot be synthesized.
    function dimOverlayColor(dim, isPaneActive, isWindowActive, base) {
        if (!(dim > 0.0) || (isPaneActive && isWindowActive))
            return "transparent"
        return Qt.alpha(base, dim)
    }

    // How strongly this pane dims while unfocused: the provider's unfocusedDim, which under Minimal floors
    // the profile's dim_unfocused because dimming is the only focus cue between panes there -- so only
    // between two or more. A lone pane dims only with its window, by the profile's own value (with the
    // Increased Contrast cap unfocusedDim applies either way: min() keeps it and drops the floor).
    // Reads the palette, hence the paletteRevision read.
    function unfocusedDim(profileDim, panes) {
        if (chromeStyle.paletteRevision < 0)
            return 0.0
        var dim = chromeStyle.unfocusedDim(profileDim)
        return panes >= 2 ? dim : Math.min(profileDim, dim)
    }

    // Unfocused-pane dimming (dim_unfocused profile option, default off; at least the kit's dim under
    // Minimal): blends the pane toward its own background color while it is not the focused pane — an
    // inactive pane of a split, or any pane of an unfocused window. Composited above the terminal
    // content; the renderer itself is untouched. Clipped like the background, so it follows the card's
    // corners and stops square at an inline header and at an ended pane's footer.
    Item {
        objectName: "dimOverlay"
        anchors.fill: parent
        anchors.margins: -pane.backgroundOutset
        anchors.bottomMargin: pane.squareBottom > 0 ? 0 : -pane.backgroundOutset
        z: 5
        clip: pane.squareTop > 0 || pane.squareBottom > 0
        property color color: pane.dimOverlayColor(
            pane.unfocusedDim(pane.session ? pane.session.dimUnfocused : 0.0, pane.visiblePanes),
            pane.paneActive,
            Window.active,
            pane.session ? pane.session.backgroundColor : "black")
        visible: color.a > 0

        Rectangle {
            anchors.fill: parent
            anchors.topMargin: -pane.squareTop
            anchors.bottomMargin: -pane.squareBottom
            radius: pane.cornerRadius
            color: parent.color
        }
    }

    TapHandler {
        onTapped: pane.activated()
    }

    // Every request the session lets through shows the pill and restarts its hold. A Connections object
    // rather than an imperative connect() in onSessionChanged: the imperative form re-connects on every
    // session rebind (a split rebinds this pane's session repeatedly) WITHOUT disconnecting the old one, so
    // stale handlers accumulate and fire on a torn-down pane during teardown. Connections re-targets
    // atomically and is torn down with the pane.
    Connections {
        target: pane.session
        ignoreUnknownSignals: true
        function onSizeIndicatorRequested() {
            pane.sizeIndicatorHeld = true
            sizeIndicatorHold.restart()
        }
    }
}
