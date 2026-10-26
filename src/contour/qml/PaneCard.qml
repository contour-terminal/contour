// vim:syntax=qml
// The kit's PaneCard: one leaf of the split tree. Once the tab shows two panes or more, the leaf is a card
// -- its corners rounded, a one-device-pixel border, e1 beneath the active one and, under Increased
// Contrast, an accent ring around it -- with its PaneHeader above the terminal (inline) or over it (the
// held-modifier reveal's overlay, which moves nothing). A tab of one pane wears none of it: the terminal is
// edge to edge.
//
// Focus is told by the header's emphasis and the inactive panes' dim, never by an outline; the Terminal
// row also lowers an inactive card's opacity (tui.inactivePaneOpacity).
//
// The card's fill is the terminal's own background, painted by the header and the terminal it holds: a
// fill of its own beneath them would composite a translucent profile twice.
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

    // A lone pane is the active one, so only a card can be inactive.
    opacity: isCard && !isActive ? chromeStyle.tui.inactivePaneOpacity : 1.0

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

    TerminalPane {
        id: terminal
        // The grid, contentInset in from the card's edges and below an inline header.
        x: card.contentInset
        y: (card.headerInline ? header.height : 0) + card.contentInset
        width: card.width - x - card.contentInset
        height: card.height - y - card.contentInset
        backgroundOutset: card.contentInset
        session: card.session
        paneActive: card.isActive
        visiblePanes: card.visiblePanes
        cornerRadius: card.cornerRadius
        squareTop: card.headerInline ? card.cornerRadius : 0
        onActivated: { if (card.node) card.node.activate() }
    }

    PaneHeader {
        id: header
        x: 0
        y: 0
        z: 1
        width: card.width
        height: card.policy.placement === "overlay" ? chromeStyle.overlayPaneHeader : chromeStyle.size.paneHeader
        visible: card.headerShown
        session: card.session
        node: card.node
        policy: card.policy
        cornerRadius: card.cornerRadius
    }

    // The card's edge, above what it holds: a border of one device pixel in the separator colour.
    Rectangle {
        objectName: "paneCardSurface"
        anchors.fill: parent
        z: 2
        visible: card.isCard
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
