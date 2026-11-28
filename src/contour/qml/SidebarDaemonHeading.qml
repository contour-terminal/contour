// vim:syntax=qml
// A daemon row's line in the sidebar: the heading of the daemon's section. In place of a workspace row's two
// parts it draws, leading, the fold's disclosure where the look's workspace rows draw a chevron, then the
// daemon's name in the look's section-heading type (chromeStyle.type.sidebarSection: the footnote's size,
// semibold, in the secondary ink on the native rows, as a source list heads a section), standing where its
// workspaces' names stand; trailing, the daemon's locality icon (this machine, or a host) where the look
// draws one, one status slot -- the link's mark (and words) while the daemon is not connected, the most
// urgent mark beneath it while it is, a still or softly pulsing mark while a jump into the section is under
// way -- and the row's buttons (Connect, Start, Cancel, Remove). On the Terminal row the name stands bold at
// column 0 with no disclosure, as the switcher's headings do, so a daemon row is never mistaken for a
// workspace row; its fold's disclosure follows the name there.
//
// What the row says and offers is the model's (window::SidebarDaemonRow.hpp, SidebarModel); how much of it
// fits is window::daemonRowFitOf() (UiStyleProvider.daemonRowFit): the locality icon goes first, then the
// secondary button, then the primary button shrinks to its icon, then the link words go into the mark's
// tooltip, and last the name elides in the middle, to its shortest; the primary button stays. Every extent, ink and
// glyph is a token.
import QtQuick
import QtQuick.Controls.impl
import Contour.Terminal

Item {
    id: root
    objectName: "sidebarDaemonHeading"

    /// The row's index in the sidebar's model.
    required property int index
    /// The daemon's name.
    required property string title
    /// Whether the section folds, and is folded (SidebarRows.RowExpansion).
    required property int expansion
    /// The daemon's link state (window::DaemonLinkState).
    required property int linkState
    /// Why its link failed, which its tooltip says; empty otherwise.
    required property string linkReason
    /// Whether the daemon is this machine's or a host's (window::DaemonLocality: Remote 0, Local 1).
    required property int locality
    /// What the trailing status slot shows (SidebarRows.StatusSlot).
    required property int statusSlot
    /// The most urgent mark beneath the row while connected (a window::TabBadgeKind).
    required property int glyph
    /// The row's buttons, leading first (SidebarRows.DaemonRowButton values; None skipped).
    required property var buttons
    /// The sidebar's model, which the buttons' actions go to.
    required property var sidebar
    /// The ink of the name, and of the marks and words beside it.
    required property color ink
    required property color secondaryInk
    /// The tint the marks sit on ({ fill, ink }), as SidebarRow.tint.
    required property var tint
    /// What the row keeps free at its leading and trailing ends.
    required property real leadPadding
    required property real trailInset

    readonly property bool cellTree: chromeStyle.sidebar.look === chromeStyle.sidebar.cellTree
    /// The look's faces of a daemon row: `{ localityIcons, pendingMark, pendingWords }`, the icons indexed by
    /// window::DaemonLocality (empty on the Terminal row).
    readonly property var face: chromeStyle.sidebarDaemonRowFace
    readonly property string localityIcon: (root.face.localityIcons ?? [])[root.locality] ?? ""
    /// The disclosure after the name, on a look whose heading draws it there (the Terminal row): the look's
    /// own fold glyphs, as its workspace rows draw them at column 0. Empty for a section with nothing to fold.
    readonly property string trailingDisclosure: !(root.face.trailingDisclosure ?? false) ? ""
        : root.expansion === SidebarRows.RowExpansion.Expanded ? chromeStyle.menuGlyph
        : root.expansion === SidebarRows.RowExpansion.Collapsed ? chromeStyle.submenuGlyph : ""
    /// How the link looks: `{ glyph, dot, words }` (window::linkLookOf()).
    readonly property var link: chromeStyle.paletteRevision < 0 ? ({}) : chromeStyle.daemonLinkLook(root.linkState)
    readonly property bool showsLink: root.statusSlot === SidebarRows.StatusSlot.Link
    readonly property bool showsUrgency: root.statusSlot === SidebarRows.StatusSlot.Urgency
    readonly property bool showsPending: root.statusSlot === SidebarRows.StatusSlot.Pending
    /// The words beside the mark: the link's, or "Connecting…" while a jump into the section is under way.
    readonly property string words: root.showsLink ? (root.link.words ?? "")
                                  : root.showsPending ? (root.face.pendingWords ?? "") : ""
    readonly property var offeredButtons: (root.buttons ?? []).filter((button) => button !== SidebarRows.DaemonRowButton.None)
    // The gap between the row's parts: a cell on the Terminal row, whose small spacing steps are no cells.
    readonly property real gap: root.cellTree ? Math.max(chromeStyle.space.s3, chromeStyle.cellWidth) : chromeStyle.space.s3

    /// How much of the row fits (SidebarRows.DaemonRowFit, window::daemonRowFitOf()).
    readonly property int fit: chromeStyle.daemonRowFit(
        root.width - root.leadPadding - root.trailInset,
        // What stands whatever the room: the disclosure, the folded mark, the locality icon, the slot's mark,
        // and the gaps between them and the name.
        (lead.implicitWidth > 0 ? lead.implicitWidth + root.gap : 0) + root.gap
            + (trailingFold.visible ? trailingFold.width + root.gap : 0)
            + (root.localityIcon !== "" ? chromeStyle.sidebar.glyph + root.gap : 0)
            + (linkMark.visible ? linkMark.width + root.gap : 0)
            + (pendingMark.visible ? pendingMark.width + root.gap : 0),
        root.localityIcon !== "" ? chromeStyle.sidebar.glyph + root.gap : 0,
        // Text extents rounded up to whole pixels, as the scene positions the parts: a fraction short, the
        // name would be cut by it. On the Terminal row to whole cells, as the parts are laid out there: a
        // pixel's rounding per part would cost the row a cell it has.
        chromeStyle.snapToCells(nameMetrics.implicitWidth),
        chromeStyle.snapToCells(elidedNameMetrics.implicitWidth),
        chromeStyle.snapToCells(shortNameMetrics.implicitWidth),
        root.words !== "" ? chromeStyle.snapToCells(wordsMetrics.implicitWidth) + root.gap : 0,
        buttonsRow.textExtent,
        buttonsRow.iconExtent,
        buttonsRow.secondaryTextExtent,
        buttonsRow.secondaryIconExtent)
    /// What the row shows at its fit: `{ wordsShown, iconButtons, secondaryShown, localityShown }`.
    readonly property var step: chromeStyle.daemonRowFitStep(root.fit)
    /// The buttons the fit keeps: the secondary one goes right after the locality icon.
    readonly property var shownButtons: root.step.secondaryShown ? root.offeredButtons : root.offeredButtons.slice(0, 1)

    // Why a link failed, and once the row has no room for them, the link's words before it: what the mark
    // means is always where the pointer rests on the row.
    KitToolTipHost {
        objectName: "sidebarDaemonReasonToolTipHost"
        text: chromeStyle.daemonRowToolTip(root.words, root.linkReason, root.fit)
        toolTipName: "sidebarDaemonReasonToolTip"
    }

    // The name's three forms and the link's words, measured as a Text draws them: TextMetrics' advance falls
    // short of what a Text takes where a glyph comes from a fallback face (the ellipsis), and the fit would
    // then promise a name the room cuts.
    Text { id: nameMetrics; visible: false; textFormat: Text.PlainText; font: nameText.font; text: root.title }
    Text {
        id: elidedNameMetrics
        visible: false
        textFormat: Text.PlainText
        font: nameText.font
        text: chromeStyle.daemonRowNameText(root.title, SidebarRows.DaemonRowFit.NameElided)
    }
    Text {
        id: shortNameMetrics
        visible: false
        textFormat: Text.PlainText
        font: nameText.font
        text: chromeStyle.daemonRowNameText(root.title, SidebarRows.DaemonRowFit.NameShort)
    }
    Text { id: wordsMetrics; visible: false; textFormat: Text.PlainText; font: wordsText.font; text: root.words }

    Row {
        id: lead
        anchors.left: parent.left
        anchors.leftMargin: root.leadPadding
        anchors.verticalCenter: parent.verticalCenter
        spacing: chromeStyle.space.s3

        // The fold, where the look's workspace rows draw a chevron; the Terminal row has none here (its
        // ▾ and ▸ are the workspaces'), and its name folds the section by click and keys alike.
        Item {
            id: disclosure
            objectName: "sidebarDaemonDisclosure"
            readonly property bool folds: root.expansion !== SidebarRows.RowExpansion.None
            readonly property bool expanded: root.expansion === SidebarRows.RowExpansion.Expanded
            visible: chromeStyle.sidebar.disclosureIcon !== ""
            anchors.verticalCenter: parent.verticalCenter
            width: chromeStyle.sidebar.glyph
            height: chromeStyle.sidebar.glyph

            IconImage {
                visible: disclosure.folds
                anchors.centerIn: parent
                width: chromeStyle.sidebar.glyph
                height: chromeStyle.sidebar.glyph
                sourceSize: Qt.size(width, height)
                source: chromeStyle.sidebar.disclosureIcon
                color: root.secondaryInk
                rotation: disclosure.expanded ? 0 : Qt.application.layoutDirection === Qt.RightToLeft ? 90 : -90
                Behavior on rotation {
                    enabled: chromeStyle.motion.animates
                    NumberAnimation { duration: chromeStyle.motion.fast }
                }
                Accessible.ignored: true
            }

            TapHandler {
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: if (root.sidebar) root.sidebar.toggle(root.index)
            }
        }
    }

    Text {
        id: nameText
        objectName: "sidebarDaemonName"
        anchors.left: lead.implicitWidth > 0 ? lead.right : parent.left
        anchors.leftMargin: lead.implicitWidth > 0 ? root.gap : root.leadPadding
        // As wide as the name, no wider than the room left of the trailing parts: the folded mark and the
        // locality icon follow it.
        width: Math.min(implicitWidth, Math.max(0, trail.x - root.gap - x
                                                   - (nameTail.implicitWidth > 0 ? nameTail.implicitWidth + root.gap : 0)))
        anchors.verticalCenter: parent.verticalCenter
        textFormat: Text.PlainText
        // Shortened in the middle once the fit says so (window::daemonRowNameText()), and by the room left
        // beyond that.
        text: chromeStyle.daemonRowNameText(root.title, root.fit)
        font: chromeStyle.type.sidebarSection
        color: (root.face.secondaryName ?? false) ? root.secondaryInk : root.ink
        elide: Text.ElideMiddle
        Accessible.ignored: true
    }

    // After the name: the disclosure on the Terminal row, and the locality's icon where the look draws one.
    Row {
        id: nameTail
        anchors.left: nameText.right
        anchors.leftMargin: nameTail.implicitWidth > 0 ? root.gap : 0
        anchors.verticalCenter: parent.verticalCenter
        spacing: root.gap

        Text {
            id: trailingFold
            objectName: "sidebarDaemonTrailingDisclosure"
            visible: root.trailingDisclosure !== ""
            anchors.verticalCenter: parent.verticalCenter
            width: chromeStyle.snapToCells(implicitWidth)
            textFormat: Text.PlainText
            text: root.trailingDisclosure
            font: chromeStyle.type.body
            color: root.secondaryInk
            Accessible.ignored: true

            TapHandler {
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: if (root.sidebar) root.sidebar.toggle(root.index)
            }
        }

        // This machine, or a host.
        IconImage {
            id: localityIcon
            objectName: "sidebarDaemonLocality"
            visible: root.localityIcon !== "" && root.step.localityShown
            anchors.verticalCenter: parent.verticalCenter
            width: chromeStyle.sidebar.glyph
            height: chromeStyle.sidebar.glyph
            sourceSize: Qt.size(width, height)
            source: root.localityIcon
            color: root.secondaryInk
            Accessible.ignored: true
        }
    }

    Row {
        id: trail
        anchors.right: parent.right
        anchors.rightMargin: root.trailInset
        anchors.verticalCenter: parent.verticalCenter
        spacing: root.gap

        // The link's mark: one cell on the Terminal row, whose marks are one-cell glyphs (window::
        // DaemonLinkLooks), however wide the face that has them draws them.
        Text {
            id: linkMark
            objectName: "sidebarDaemonLinkMark"
            visible: root.showsLink
            anchors.verticalCenter: parent.verticalCenter
            width: root.cellTree ? chromeStyle.cellWidth : chromeStyle.snapToCells(implicitWidth)
            horizontalAlignment: Text.AlignHCenter
            textFormat: Text.PlainText
            text: root.link.glyph ?? ""
            font: chromeStyle.type.body
            color: root.link.dot === undefined ? root.secondaryInk
                 : root.tint.fill !== undefined ? chromeStyle.sidebarMarkInk(root.link.dot, root.tint.fill, root.tint.ink)
                 : root.link.dot
            Accessible.ignored: true
        }

        // A jump into the section under way.
        SidebarPendingMark {
            id: pendingMark
            pending: root.showsPending
            anchors.verticalCenter: parent.verticalCenter
            color: root.secondaryInk
        }

        Text {
            id: wordsText
            objectName: "sidebarDaemonLinkWords"
            visible: root.words !== "" && root.step.wordsShown
            anchors.verticalCenter: parent.verticalCenter
            width: chromeStyle.snapToCells(implicitWidth)
            textFormat: Text.PlainText
            text: root.words
            font: chromeStyle.type.footnote
            color: root.secondaryInk
            Accessible.ignored: true
        }

        // The most urgent mark beneath a connected daemon.
        TabBadgeSlot {
            objectName: "sidebarDaemonUrgency"
            visible: root.showsUrgency && known
            anchors.verticalCenter: parent.verticalCenter
            width: chromeStyle.snapToCells(implicitWidth)
            kind: root.glyph
            shaped: !root.cellTree
            tint: root.tint
        }

        Row {
            id: buttonsRow
            anchors.verticalCenter: parent.verticalCenter
            // A cell between them on the Terminal row, whose text buttons would otherwise read as one word.
            spacing: root.cellTree ? root.gap : chromeStyle.space.s2
            /// What the offered buttons take with their words, and in their icon forms, gaps included; and what
            /// the secondary one takes of each, its gap included (0 for a row with one button).
            readonly property real textExtent: buttonsRow.sum((button) => button.textExtent)
            readonly property real iconExtent: buttonsRow.sum((button) => button.iconExtent)
            readonly property var secondary: root.offeredButtons.length > 1 && measured.count > 1 ? measured.itemAt(1) : null
            readonly property real secondaryTextExtent: buttonsRow.secondary ? buttonsRow.secondary.textExtent + buttonsRow.spacing : 0
            readonly property real secondaryIconExtent: buttonsRow.secondary ? buttonsRow.secondary.iconExtent + buttonsRow.spacing : 0

            function sum(extentOf) {
                let total = 0
                for (let i = 0; i < measured.count; ++i) {
                    const button = measured.itemAt(i)
                    if (button)
                        total += extentOf(button) + (i > 0 ? buttonsRow.spacing : 0)
                }
                return total
            }

            // Every offered button, measured whatever the fit shows: the fit is asked what they take.
            Repeater {
                id: measured
                model: root.offeredButtons
                delegate: SidebarDaemonButton {
                    required property int modelData
                    objectName: "sidebarDaemonButtonMeasure"
                    button: modelData
                    visible: false
                    extent: Math.min(chromeStyle.size.toolbarButton, root.height)
                }
            }

            Repeater {
                id: buttonRepeater
                model: root.shownButtons
                delegate: SidebarDaemonButton {
                    required property int modelData
                    button: modelData
                    pending: root.showsPending ? SidebarRows.RowPending.Yes : SidebarRows.RowPending.No
                    iconWanted: root.step.iconButtons
                    extent: Math.min(chromeStyle.size.toolbarButton, root.height)
                    glyphColor: root.ink
                    // What the button does is the model's: the row only says which it was.
                    onClicked: if (root.sidebar) root.sidebar.pressDaemonButton(root.index, modelData)
                }
            }
        }
    }
}
