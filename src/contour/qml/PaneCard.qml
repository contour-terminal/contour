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
// An ended pane, or a restored shell over the command a restart stopped, shows its ExitedFooter below the
// cells, which end where it starts; a pane whose host
// connection is being sought again dims below its header and shows a pill naming the host.
//
// The card's fill is the terminal's own background, painted by the header and the terminal it holds: a
// fill of its own beneath them would composite a translucent profile twice.
//
// Something dragged over the card is taken or left alone by its DropArea, as WindowController.dropZoneAt
// decides: files and text insert at the prompt, a tab dropped on an edge band splits the pane, and anything
// else stays unaccepted -- a tab dropped in the middle ends as it did before, torn off into its own window.
// While a drag it would take hovers the card, a ring around it and, for a split, the half the tab would
// take say so.
//
// Three things here sit over cells, against the spec's "nothing goes over non-blank cells" (§1.5), and all
// are ruled exceptions, not precedent: the drop ring and ghost, for the length of a drag (Q3); the
// held-modifier reveal's overlay header (D4), which must move nothing; and the reconnecting pill over a grid
// that is frozen and dimmed anyway. Anything else that would cover cells (Step 3f's sticky header among them)
// takes room of its own, as the inline header and the ExitedFooter do.
import QtQuick
import QtQuick.Window

Item {
    id: card
    objectName: "paneCard"

    /// The leaf's PaneProxy; null while a split rebinds.
    property var node: null
    /// The panes the tab shows (WindowController.visiblePaneCount).
    property int visiblePanes: 1
    /// Whether a locked pane without a header draws its lock in a pill over its grid (PaneNode.lockInPane). A
    /// pane whose header is shown on screen now holds the lock there instead, so the card drops it.
    property bool lockInPane: false
    /// A window::HeaderReveal's value (WindowController.headerReveal).
    property int headerReveal: 0
    /// A window::TitleBandPresence's value: whether the window's band shows the title (PaneNode.titleBand).
    /// 0 is TitleBandPresence::Absent, as chromeStyle.blocks.bandAbsent.
    property int titleBand: 0

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

    // Where the pane's sticky command header lives (window::stickyHeaderHostFor): in this card's header while
    // it shows one, else in the title band for the tab's only pane, else over the grid. The session keeps it
    // and shows or withholds the header in its chrome accordingly.
    readonly property int stickyHeaderHost: chromeStyle.stickyHeaderHost(
        headerShown ? chromeStyle.blocks.headerShown : chromeStyle.blocks.headerHidden, visiblePanes, titleBand)
    onStickyHeaderHostChanged: card.announceStickyHeaderHost()
    onSessionChanged: card.announceStickyHeaderHost()
    Component.onCompleted: card.announceStickyHeaderHost()

    function announceStickyHeaderHost() {
        // Guarded as the link pill is: a session stand-in without the member has no header to place.
        if (session && typeof session.setStickyHeaderHost === "function")
            session.setStickyHeaderHost(card.stickyHeaderHost)
    }

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
            lockInPane: card.lockInPane && !card.headerShown
            cornerRadius: card.cornerRadius
            squareTop: card.headerInline ? card.cornerRadius : 0
            squareBottom: footer.visible ? card.cornerRadius : 0
            onActivated: { if (card.node) card.node.activate() }
            // The find bar's room in the header row, and whether the pane has an inline header to give it
            // (F5: not a Minimal overlay header, which comes and goes with a held key).
            headerFindSlot: header.findSlot
            headerPresent: card.headerInline ? 1 : 0
            overlayHost: paneOverlays
            // The header's trailing area hosts the link pill only while the header is inline: Minimal's header
            // revealed over the grid covers cells, so the pill keeps to the grid then.
            headerLinkSlot: header.linkPillSlot
        }
    }

    // Laid exactly over the grid but outside the content's fade: the pane's own overlay (the size indicator)
    // reads at full strength on an inactive Terminal-row card.
    Item {
        id: paneOverlays
        objectName: "paneCardOverlays"
        x: terminal.x
        y: terminal.y
        z: 1
        width: terminal.width
        height: terminal.height
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
            elevation: chromeStyle.elevation.inPane
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
        // The find bar takes the trailing area before the link pill: while the bar is in this row, the pill is
        // placed elsewhere (window::placeLinkPill).
        trailingOccupant: terminal.findInHeaderRow ? chromeStyle.headerOccupants.find
                        : terminal.linkPillInHeader ? chromeStyle.headerOccupants.linkPill
                        : chromeStyle.headerOccupants.none
        // A hidden header button drops the keyboard focus; typing goes back to this pane's grid.
        onFocusReleased: terminal.forceActiveFocus()
    }

    // The pane's footer: how it ended with Restart where it may and Close, or the command a restart stopped
    // with Run Again and Dismiss.
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

    // What dragged over the card would do. A tab rides in the drag's mime data (TabItem's dragProxy); the
    // controller resolves the payload it names against the model, so nothing here trusts it.
    DropArea {
        id: paneDrop
        objectName: "paneDropArea"
        anchors.fill: parent
        z: 4

        /// The mime format a tab drag carries (TabItem.qml's Drag.mimeData, TabStrip.qml's keys).
        readonly property string tabFormat: "application/x-contour-tab"
        /// The window's controller (Main.qml's `win`); null while the window is torn down.
        readonly property var controller: Window.window && Window.window.win ? Window.window.win : null
        /// What is dragged, as a chromeStyle.dropPayloads value, and the tab JSON when it is a tab.
        property int payload: chromeStyle.dropPayloads.none
        property string tabPayload: ""
        /// What a drop here would do, as a chromeStyle.dropZones value.
        property int zone: chromeStyle.dropZones.none
        /// The half of the card a split would give the tab; a null rectangle for any other zone.
        readonly property rect ghost: controller ? controller.dropGhostRect(zone, card.width, card.height,
                                                                            chromeStyle.space.s2)
                                                 : Qt.rect(0, 0, 0, 0)
        readonly property bool splits: ghost.width > 0
        /// Whether the card takes the drag as it is now: text and files insert, a tab splits on an edge. The
        /// middle of a card (PassThrough) and anything it does not understand are left unaccepted.
        readonly property bool takes: zone === chromeStyle.dropZones.insertText || splits

        function payloadOf(drag) {
            if (drag.formats.indexOf(tabFormat) >= 0)
                return chromeStyle.dropPayloads.tab
            // Every URL list is the session's to type (files and web links alike, escaped and checked there):
            // a page chooses what its drag carries, so none of it is pasted here as text.
            if (drag.hasUrls)
                return chromeStyle.dropPayloads.files
            if (drag.hasText)
                return chromeStyle.dropPayloads.text
            return chromeStyle.dropPayloads.none
        }

        function refresh(drag) {
            payload = payloadOf(drag)
            tabPayload = payload === chromeStyle.dropPayloads.tab ? drag.getDataAsString(tabFormat) : ""
            // Fail closed: a call the engine could not make answers `undefined`, which is no zone at all.
            const answer = controller && card.session
                           ? controller.dropZoneAt(card.session, payload, drag.x, drag.y, card.width,
                                                   card.height, tabPayload)
                           : chromeStyle.dropZones.none
            zone = typeof answer === "number" ? answer : chromeStyle.dropZones.none
        }

        function reset() {
            payload = chromeStyle.dropPayloads.none
            tabPayload = ""
            zone = chromeStyle.dropZones.none
        }

        // A tab is accepted wherever it lands on the card, so the drag keeps reporting its position into the
        // edge bands; the middle is turned away at the drop. Anything the card cannot place is refused here.
        onEntered: (drag) => {
            refresh(drag)
            drag.accepted = zone !== chromeStyle.dropZones.none
        }
        onPositionChanged: (drag) => refresh(drag)
        onExited: reset()
        onDropped: (drop) => {
            refresh(drop)
            const dropped = { zone: zone, payload: payload, tabPayload: tabPayload, takes: takes, splits: splits }
            reset()
            if (!dropped.takes || !card.session || !controller) {
                // Not accepted, so the drag ends IgnoreAction: a tab then tears off, as it always did.
                drop.accepted = false
                return
            }
            if (dropped.splits) {
                controller.dropTabOnPane(card.session, dropped.zone, dropped.tabPayload)
                drop.accept(Qt.MoveAction)
                return
            }
            // URLs are typed by the session as quoted words; plain text takes the paste path of Ctrl+V.
            if (dropped.payload === chromeStyle.dropPayloads.files)
                card.session.insertDroppedUrls(drop.urls)
            else
                card.session.insertDroppedText(drop.text)
            drop.accept(Qt.CopyAction)
        }
    }

    // The mark of a drop: the ring around the card and the ghost's edge. The one colour the overlay
    // scrollbar's engaged thumb is, resolved on the card's own fill, which holds 3:1 on every row,
    // appearance and bundled scheme (UiStyleProvider_test).
    readonly property color dropMark: chromeStyle.paletteRevision < 0
                                      ? "transparent"
                                      : chromeStyle.colorOn(chromeStyle.roles.scrollbarThumbActive, card.fill)

    // The ring says the card will take what hovers it.
    Rectangle {
        objectName: "dropRing"
        anchors.fill: parent
        z: 4
        visible: paneDrop.containsDrag && paneDrop.takes
        radius: card.cornerRadius
        color: "transparent"
        border.width: chromeStyle.space.s1
        border.color: card.dropMark
    }

    // The ghost shows the half a tab would take, and says what it would do, on a toast's own contrast.
    Rectangle {
        objectName: "dropGhost"
        x: paneDrop.ghost.x
        y: paneDrop.ghost.y
        width: paneDrop.ghost.width
        height: paneDrop.ghost.height
        z: 4
        visible: paneDrop.containsDrag && paneDrop.splits
        radius: card.cornerRadius
        color: "transparent"
        border.width: chromeStyle.space.s1
        border.color: card.dropMark

        Toast {
            objectName: "dropGhostLabel"
            anchors.centerIn: parent
            shown: parent.visible
            elevation: chromeStyle.elevation.inPane
            text: paneDrop.controller ? paneDrop.controller.dropGhostLabel(paneDrop.zone) : ""
        }
    }
}
