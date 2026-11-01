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
// cells, which end where it starts; so does the shell integration offer, in a band of its own that an
// ended pane's footer replaces; a pane whose host
// connection is being sought again dims below its header and shows a pill naming the host.
//
// The card's fill is the terminal's own background, painted by the header and the terminal it holds: a
// fill of its own beneath them would composite a translucent profile twice.
//
// Something dragged over the card is taken or left alone by its DropArea, as WindowController.dropZoneAt
// decides: files and text insert at the prompt; a pane or tab (`application/x-contour-node`) dropped on an
// edge band splits the pane, and a pane dropped in the middle trades places with it. Every such drop the
// card recognises is accepted -- a no-op over the pane's own card, a refusal where the window cannot take it
// -- so only a drop no window recognises ends as a drop outside every window. A tab dropped in the middle is
// left to the window, which takes it as a no-op. While a drag the card takes hovers it, a ring around it and,
// for a split or a swap, the part of the pane that would be taken say so, with a label; a refused drag shows
// the reason in a note, and its ring in the warning role.
//
// The card is also the pane's own drag source (its header, PaneHeader.qml): while the pane is carried, the
// card shows `<name>'s place` in a dashed outline in place of its content.
//
// Three things here sit over cells, against the spec's "nothing goes over non-blank cells" (§1.5), and all
// are ruled exceptions, not precedent: the drop ring and ghost, for the length of a drag (Q3); the
// held-modifier reveal's overlay header (D4), which must move nothing; and the reconnecting pill over a grid
// that is frozen and dimmed anyway. Anything else that would cover cells (Step 3f's sticky header among them)
// takes room of its own, as the inline header and the ExitedFooter do.
import QtQuick
import QtQuick.Window
import Contour.Terminal

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

    /// Whether this pane is being carried by its header's drag: the window's controller says, as the drag is
    /// the window's for its length.
    readonly property bool dragging: !!paneDrop.controller && !!card.session
                                     && paneDrop.controller.carriedPane === card.session

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
            height: content.height - y - (footer.visible ? footer.height
                                          : shellBand.visible ? shellBand.height + card.contentInset
                                          : card.contentInset)
            backgroundOutset: card.contentInset
            session: card.session
            paneActive: card.isActive
            visiblePanes: card.visiblePanes
            lockInPane: card.lockInPane && !card.headerShown
            cornerRadius: card.cornerRadius
            squareTop: card.headerInline ? card.cornerRadius : 0
            squareBottom: footer.visible || shellBand.visible ? card.cornerRadius : 0
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

    // The shell integration offer's band: the card below the cells, which end where the band starts. Clipped to
    // the band, so the card's shadow never reaches over the cells above it.
    Item {
        id: shellBand
        objectName: "shellIntegrationBand"
        x: 0
        y: card.height - card.contentInset - height
        z: 1
        width: card.width
        height: shellCard.shown ? shellCard.height + shellBandGap + shellBandGap : 0
        visible: shellCard.shown
        clip: true
        // The room above and below the card: none on the Terminal row, whose card is whole cell rows.
        readonly property real shellBandGap: chromeStyle.chipOpenGlyph !== "" ? 0 : chromeStyle.space.s4

        // The pane's background at the profile's opacity, as the terminal above it is; its bottom corners
        // follow the card.
        Item {
            anchors.fill: parent
            clip: card.cornerRadius > 0

            Rectangle {
                objectName: "shellIntegrationBandBackground"
                anchors.fill: parent
                anchors.topMargin: -card.cornerRadius
                radius: card.cornerRadius
                color: card.fill
                opacity: card.session ? card.session.opacity : 1.0
            }
        }

        ShellIntegrationCard {
            id: shellCard
            x: (parent.width - width) / 2
            y: shellBand.shellBandGap
            session: card.session
        }
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

    // The empty place a carried pane leaves: its card, dashed, and whose place it is. Above the content and
    // the header, under the drop marks, so a pane carried over its own card still shows where it was.
    Item {
        id: placeholder
        objectName: "dragPlaceholder"
        anchors.fill: parent
        z: 3
        visible: card.dragging

        Rectangle {
            anchors.fill: parent
            radius: card.cornerRadius
            color: card.fill
        }

        DashedFrame {
            anchors.fill: parent
            anchors.margins: chromeStyle.space.s1
            cornerInset: card.cornerRadius
            color: chromeStyle.paletteRevision < 0 ? "transparent"
                 : chromeStyle.colorOn(chromeStyle.roles.paneCardBorder, card.fill)
        }

        Text {
            objectName: "dragPlaceholderLabel"
            anchors.centerIn: parent
            width: parent.width - chromeStyle.space.s5 - chromeStyle.space.s5
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: qsTr("%1's place").arg(header.shownName)
            font: chromeStyle.type.footnote
            color: header.dimmedText
        }
    }

    // What dragged over the card would do. A pane or tab rides in the drag's mime data
    // (PaneHeader.qml's and TabItem.qml's drag proxies); the controller resolves the payload it names against
    // the model, so nothing here trusts it.
    DropArea {
        id: paneDrop
        objectName: "paneDropArea"
        anchors.fill: parent
        z: 4

        /// The mime format a pane or tab drag carries.
        readonly property string nodeFormat: NodeDrag.mime
        /// The window's controller (Main.qml's `win`); null while the window is torn down.
        readonly property var controller: Window.window && Window.window.win ? Window.window.win : null
        /// What is dragged, as a chromeStyle.dropPayloads value, and the node JSON when it is a pane or tab.
        property int payload: chromeStyle.dropPayloads.none
        property string nodePayload: ""
        /// What a drop here would do, as a chromeStyle.dropZones value.
        property int zone: chromeStyle.dropZones.none
        /// Whether a pane or tab is dragged over the card (rather than files or text).
        readonly property bool carriesNode: payload === chromeStyle.dropPayloads.pane
                                            || payload === chromeStyle.dropPayloads.tab
        /// Where the pane starts beneath an inline header, which the ghost keeps clear of.
        readonly property real bodyTop: card.headerInline ? header.height : 0
        /// Whether the drag swaps the two panes: the whole pane below the header is taken.
        readonly property bool swaps: zone === chromeStyle.dropZones.swap
        /// The part of the pane below the header a split would give the dragged node, or the whole of it for
        /// a swap; a null rectangle for any other zone.
        readonly property rect ghost: {
            if (!controller)
                return Qt.rect(0, 0, 0, 0)
            const inset = chromeStyle.space.s2
            if (swaps)
                return Qt.rect(inset, bodyTop + inset, card.width - 2 * inset, card.height - bodyTop - 2 * inset)
            const half = controller.dropGhostRect(zone, card.width, card.height - bodyTop, inset)
            return half.width > 0 ? Qt.rect(half.x, half.y + bodyTop, half.width, half.height)
                                  : Qt.rect(0, 0, 0, 0)
        }
        readonly property bool splits: ghost.width > 0
        /// Whether a pane or tab is dragged over a place the window refuses to move it to.
        readonly property bool refuses: carriesNode && zone === chromeStyle.dropZones.refused
        /// Whether the card takes the drag as it is now: text and files insert, and a pane or tab it has a
        /// zone for -- a split, a swap, a no-op over itself, a refusal -- is taken. The middle of a card
        /// under a tab (PassThrough) and anything it does not understand are left unaccepted.
        readonly property bool takes: zone === chromeStyle.dropZones.insertText
                                      || (carriesNode && zone !== chromeStyle.dropZones.none
                                          && zone !== chromeStyle.dropZones.passThrough)

        function payloadOf(drag) {
            if (drag.formats.indexOf(nodeFormat) >= 0) {
                // The kind the JSON says it is: a tab of one pane drags as the pane. Fail closed.
                const kind = controller ? controller.dropPayloadOf(drag.getDataAsString(nodeFormat)) : undefined
                return typeof kind === "number" ? kind : chromeStyle.dropPayloads.none
            }
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
            nodePayload = carriesNode ? drag.getDataAsString(nodeFormat) : ""
            // Fail closed: a call the engine could not make answers `undefined`, which is no zone at all.
            const answer = controller && card.session
                           ? controller.dropZoneAt(card.session, payload, drag.x, drag.y, card.width,
                                                   card.height, nodePayload)
                           : chromeStyle.dropZones.none
            zone = typeof answer === "number" ? answer : chromeStyle.dropZones.none
        }

        function reset() {
            payload = chromeStyle.dropPayloads.none
            nodePayload = ""
            zone = chromeStyle.dropZones.none
        }

        // A drag is accepted on entry wherever it lands on the card, so it keeps reporting its position into
        // the edge bands; anything the card cannot place at all is refused here. From then on every move is
        // answered with what a drop there would do, so the cursor says "forbidden" over the middle of a tab
        // drag and "accepting" over an edge.
        onEntered: (drag) => {
            refresh(drag)
            drag.accepted = zone !== chromeStyle.dropZones.none
        }
        onPositionChanged: (drag) => {
            refresh(drag)
            drag.accepted = takes
        }
        onExited: reset()
        onDropped: (drop) => {
            refresh(drop)
            const dropped = { zone: zone, payload: payload, nodePayload: nodePayload, takes: takes,
                              carriesNode: carriesNode }
            reset()
            if (!dropped.takes || !card.session || !controller) {
                // Not accepted: the window may take it (a tab in the middle of a card is a no-op there), or
                // it is a drop no window recognises.
                drop.accepted = false
                return
            }
            if (dropped.carriesNode) {
                // Splits and swaps move; a no-op or a refusal moves nothing, and is accepted all the same,
                // or the drag would end as a drop outside every window and open one.
                const moves = dropped.zone === chromeStyle.dropZones.splitLeft
                              || dropped.zone === chromeStyle.dropZones.splitRight
                              || dropped.zone === chromeStyle.dropZones.splitTop
                              || dropped.zone === chromeStyle.dropZones.splitBottom
                              || dropped.zone === chromeStyle.dropZones.swap
                if (moves)
                    controller.dropNodeOnPane(card.session, dropped.zone, dropped.nodePayload)
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

    // The mark of a drop: the ring around the card, the ghost's fill and edge and its label's pill. Every
    // "will land here" cue of the design is the chrome's accent.
    readonly property color dropMark: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.accentColor

    // The warning a refusal is drawn in, on the card's own fill: the chrome's warning mark.
    readonly property color refusalMark: chromeStyle.paletteRevision < 0
                                         ? "transparent"
                                         : chromeStyle.statusMarkOn(chromeStyle.statusWarning, card.fill)

    // The ring says the card will take what hovers it; in the warning role it says the window will not move
    // the node here. Not over the pane's own card, where the drop changes nothing.
    Rectangle {
        objectName: "dropRing"
        anchors.fill: parent
        z: 4
        visible: paneDrop.containsDrag && (paneDrop.refuses || (paneDrop.takes && paneDrop.zone !== chromeStyle.dropZones.noOp))
        radius: card.cornerRadius
        color: "transparent"
        border.width: chromeStyle.dropRingWidth // the design's ring: 2 px
        border.color: paneDrop.refuses ? card.refusalMark : card.dropMark
    }

    // The ghost shows the part of the pane a split or a swap would give the dragged node, and says what it
    // would do in a pill of the accent: the accent at 18 %, edged 1.5 px in the accent, as the design draws it.
    Rectangle {
        objectName: "dropGhost"
        x: paneDrop.ghost.x
        y: paneDrop.ghost.y
        width: paneDrop.ghost.width
        height: paneDrop.ghost.height
        z: 4
        visible: paneDrop.containsDrag && (paneDrop.splits || paneDrop.swaps)
        radius: card.cornerRadius
        color: Qt.alpha(card.dropMark, 0.18)
        border.width: chromeStyle.dropEdgeWidth // the design's ghost edge: 1.5 px
        border.color: card.dropMark

        Rectangle {
            id: ghostPill
            objectName: "dropGhostPill"
            anchors.centerIn: parent
            width: Math.min(ghostText.implicitWidth + chromeStyle.space.s5 + chromeStyle.space.s5, parent.width)
            height: ghostText.implicitHeight + chromeStyle.space.s3 + chromeStyle.space.s3
            radius: chromeStyle.radii.toast
            color: card.dropMark

            Text {
                id: ghostText
                objectName: "dropGhostLabel"
                anchors.centerIn: parent
                width: parent.width - chromeStyle.space.s5 - chromeStyle.space.s5
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: ghostPill.visible && paneDrop.controller
                      ? paneDrop.controller.dropGhostLabel(paneDrop.zone, paneDrop.payload, card.session) : ""
                font: chromeStyle.activeTabFont(chromeStyle.type.footnote)
                color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.onColor(card.dropMark)
            }
        }
    }

    // Why the window will not take the dragged node: a note over the pane, in the warning role, naming what
    // the drag can be moved to instead.
    Rectangle {
        id: refusalNote
        objectName: "dropRefusal"
        z: 4
        x: chromeStyle.space.s7
        y: paneDrop.bodyTop + chromeStyle.space.s8
        width: card.width - x - x
        height: refusalColumn.implicitHeight + chromeStyle.space.s5 + chromeStyle.space.s5
        visible: paneDrop.containsDrag && paneDrop.refuses
        radius: chromeStyle.radii.toast
        color: chromeStyle.paletteRevision < 0 ? "transparent" : Qt.tint(card.fill, Qt.alpha(card.refusalMark, 0.18))
        border.width: chromeStyle.hairline(Screen.devicePixelRatio)
        border.color: card.refusalMark
        Accessible.role: Accessible.AlertMessage
        Accessible.name: refusalTitle.text + ", " + refusalBody.text

        Column {
            id: refusalColumn
            x: chromeStyle.space.s5
            y: chromeStyle.space.s5
            width: parent.width - x - x
            spacing: chromeStyle.space.s2

            Text {
                id: refusalTitle
                objectName: "dropRefusalTitle"
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Can't move a pane here")
                // The face a row sets its emphasis apart with (bold, or the Terminal row's own).
                font: chromeStyle.activeTabFont(chromeStyle.type.footnote)
                color: chromeStyle.paletteRevision < 0 ? "transparent"
                     : chromeStyle.statusTextOn(chromeStyle.statusWarning, refusalNote.color)
            }

            Text {
                id: refusalBody
                objectName: "dropRefusalBody"
                width: parent.width
                wrapMode: Text.WordWrap
                text: paneDrop.controller && paneDrop.refuses ? paneDrop.controller.refusalText(paneDrop.nodePayload) : ""
                font: chromeStyle.type.footnote
                color: chromeStyle.paletteRevision < 0 ? "transparent"
                     : chromeStyle.colorOn(chromeStyle.roles.paneHeaderText, refusalNote.color)
            }
        }
    }
}
