// vim:syntax=qml
// The kit's PaneCard: one leaf of the split tree. Once the tab shows two panes or more, the leaf is a card
// -- its corners rounded, a one-device-pixel border (Comfortable only), e1 beneath the active one and,
// under Increased Contrast, an accent ring around it -- with its PaneHeader above the terminal (inline) or
// over it (the held-modifier reveal's overlay, which moves nothing). A tab of one pane wears none of it:
// the terminal is edge to edge.
//
// Focus is told by the header's emphasis and the inactive panes' dim, never by an outline; the Terminal
// row also lowers an inactive card's content (tui.inactivePaneOpacity; a lone pane is always active, so
// only a card can be inactive), not its header.
//
// An ended pane shows its ExitedFooter below the cells, which end where it starts; a pane whose host
// connection is being sought again dims below its header and shows a pill naming the host.
//
// The card's fill is the terminal's own background, painted by the header and the terminal it holds: a
// fill of its own beneath them would composite a translucent profile twice.
//
// Two things here sit over cells, against the spec's "nothing goes over non-blank cells" (§1.5), and both
// are ruled exceptions, not precedent: the held-modifier reveal's overlay header (D4), which must move
// nothing, and the reconnecting pill over a grid that is frozen and dimmed anyway. Anything else that would
// cover cells (Step 3f's sticky header among them) takes room of its own, as the inline header and the
// ExitedFooter do.
import QtQuick
import QtQuick.Window

Item {
    id: card
    objectName: "paneCard"

    /// The leaf's PaneProxy; null while a split rebinds.
    property var node: null
    /// The panes the tab shows (WindowController.visiblePaneCount).
    property int visiblePanes: 1
    /// A window::HeaderReveal's value (WindowController.headerReveal).
    property int headerReveal: 0

    readonly property var session: node ? node.session : null
    readonly property bool isActive: node ? node.active : false
    readonly property bool isCard: visiblePanes >= 2
    readonly property color fill: session ? session.backgroundColor : "black"
    readonly property real cornerRadius: isCard ? chromeStyle.radii.paneCard : 0
    // How far the grid sits in from every edge of a rounded card, so that no cell reaches its corners; the
    // window-size solver counts it (window::splitChromeOf).
    readonly property real contentInset: isCard ? chromeStyle.paneCardContentInset(Screen.devicePixelRatio) : 0

    // The provider's decision for this pane's header; an empty map (a value out of range) hides it.
    readonly property var policy: chromeStyle.paneHeader(headerReveal,
                                                         hover.hovered ? 1 : 0,
                                                         node ? node.focus : 0,
                                                         visiblePanes,
                                                         width,
                                                         height)
    readonly property bool headerShown: policy.content !== undefined && policy.content !== "hidden"
    readonly property bool headerInline: headerShown && policy.placement === "inline"

    HoverHandler {
        id: hover
    }

    ElevationShadow {
        objectName: "paneCardShadow"
        anchors.fill: parent
        // A soft shadow only: the Terminal row's hard offset copy would shade the gap beside the card.
        visible: card.isCard && card.isActive && chromeStyle.elevation.e1.blur > 0
        level: chromeStyle.elevation.e1
        radius: card.cornerRadius
    }

    // The pane's content: what the Terminal row fades on an inactive card, its header staying legible.
    Item {
        id: content
        objectName: "paneCardContent"
        anchors.fill: parent
        opacity: card.isCard && !card.isActive ? chromeStyle.tui.inactivePaneOpacity : 1.0

        TerminalPane {
            id: terminal
            // The grid, contentInset in from the card's edges, below an inline header and above an ended
            // pane's footer, which it meets edge to edge: the footer is never over cells.
            x: card.contentInset
            y: (card.headerInline ? header.height : 0) + card.contentInset
            width: content.width - x - card.contentInset
            height: content.height - y - (footer.visible ? footer.height : card.contentInset)
            backgroundOutset: card.contentInset
            session: card.session
            paneActive: card.isActive
            visiblePanes: card.visiblePanes
            cornerRadius: card.cornerRadius
            squareTop: card.headerInline ? card.cornerRadius : 0
            squareBottom: footer.visible ? card.cornerRadius : 0
            onActivated: { if (card.node) card.node.activate() }
            // The find bar's room in the header row, and whether the pane has an inline header to give it
            // (F5: not a Minimal overlay header, which comes and goes with a held key).
            headerFindSlot: header.findSlot
            headerPresent: card.headerInline ? 1 : 0
        }
    }

    // While the connection to the host this pane mirrors is sought again, the grid dims by the kit's pane
    // dim and a pill says so, centred on it. The window's host chip says the same of the whole window
    // (Reconnecting…); this names the host where there is one, and nothing here contradicts it. Declared
    // before the header at its z, so a header revealed over the grid (Minimal) stays above the dim; not
    // over an ended pane, whose footer already says how it stands.
    Item {
        id: reconnecting
        objectName: "reconnectingOverlay"
        x: 0
        y: card.headerInline ? header.height : 0
        z: 1
        width: card.width
        height: card.height - y
        visible: card.session ? card.session.paneState === chromeStyle.paneStates.reconnecting : false
        clip: card.headerInline && card.cornerRadius > 0
        // The dim's colour, for the scene's own checks.
        readonly property color color: chromeStyle.paneDimColor
        // The window's controller (Main.qml's `win`), which names the host; empty for the local daemon.
        readonly property var controller: Window.window && Window.window.win ? Window.window.win : null
        readonly property string hostLabel: controller ? controller.hostLabel : ""

        // The card's corners below an inline header, square where it meets the header.
        Rectangle {
            anchors.fill: parent
            anchors.topMargin: card.headerInline ? -card.cornerRadius : 0
            radius: card.cornerRadius
            color: reconnecting.color
        }

        // Its glyph is the Working badge's, drawn still in the warning mark ink of the host chip's
        // Reconnecting dot, resolved on the pill's own fill (on the Terminal row, the scheme inverted): the
        // pill is a steady state, not a progress. Nor is it a popover, so it casts a shadow only where the
        // row's shadow is soft; the Terminal row's hard offset copy would read as a second row beneath it.
        Toast {
            objectName: "reconnectingPill"
            anchors.centerIn: parent
            shown: true
            elevation: chromeStyle.elevation.e2.blur > 0 ? chromeStyle.elevation.e2 : chromeStyle.elevation.e0
            text: reconnecting.hostLabel !== "" ? qsTr("Reconnecting to %1…").arg(reconnecting.hostLabel)
                                                : qsTr("Reconnecting…")
            glyph: chromeStyle.paletteRevision < 0 ? "" : chromeStyle.badgeLook(chromeStyle.badgeKinds.working).glyph
            glyphColor: chromeStyle.paletteRevision < 0 ? "transparent"
                      : chromeStyle.statusMarkOn(chromeStyle.statusWarning, chromeStyle.colors.toastFill)
        }
    }

    PaneHeader {
        id: header
        x: 0
        y: 0
        z: 1
        width: card.width
        // Inline, it is floored to whole device pixels, so the grid below it starts on the device grid
        // (#2040) -- as the window-size solver counts it (window::splitChromeOf). The overlay moves nothing.
        height: card.policy.placement === "overlay" ? chromeStyle.overlayPaneHeader
                                                    : chromeStyle.paneHeaderHeight(Screen.devicePixelRatio)
        visible: card.headerShown
        session: card.session
        node: card.node
        policy: card.policy
        cornerRadius: card.cornerRadius
        findActive: terminal.findInHeaderRow
        // A hidden header button drops the keyboard focus; typing goes back to this pane's grid.
        onFocusReleased: terminal.forceActiveFocus()
    }

    // An ended pane's footer: how it ended, Restart where it may, and Close.
    ExitedFooter {
        id: footer
        x: 0
        y: card.height - height
        z: 1
        width: card.width
        session: card.session
        cornerRadius: card.cornerRadius
        // A click on the footer, as on the terminal, makes the ended pane the active one: Return then
        // restarts it.
        onActivated: { if (card.node) card.node.activate() }
    }

    // The card's edge, above what it holds: a border of one device pixel in the separator colour. Minimal
    // frames no card: the gap's one line between the panes is all it draws, so no pane is outlined at the
    // window's edge.
    Rectangle {
        objectName: "paneCardSurface"
        anchors.fill: parent
        z: 2
        visible: card.isCard && chromeStyle.density !== "minimal"
        radius: card.cornerRadius
        color: "transparent"
        border.width: chromeStyle.hairline(Screen.devicePixelRatio)
        border.color: chromeStyle.paletteRevision < 0 ? "transparent"
                    : chromeStyle.colorOn(chromeStyle.roles.paneCardBorder, card.fill)
    }

    // Increased Contrast's ring around the active card, in the accent; contrast.activeCardRing is 0 else.
    Rectangle {
        objectName: "activeCardRing"
        anchors.fill: parent
        z: 3
        visible: card.isCard && card.isActive && chromeStyle.contrast.activeCardRing > 0
        radius: card.cornerRadius
        color: "transparent"
        border.width: chromeStyle.contrast.activeCardRing
        border.color: chromeStyle.accentColor
    }
}
