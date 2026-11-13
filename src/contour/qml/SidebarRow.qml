// vim:syntax=qml
// One row of the workspace sidebar (SidebarModel): a workspace, a tab of one, or a pane of a split tab.
//
// A workspace row: its disclosure, its name, its git state and the rolled-up status glyph. A tab or pane row:
// its status glyph, its title, its reason on a second line when there is one, and its time -- or its progress,
// as a thin meter -- at the right. Everything the row says, and in what order the statuses rank, is the
// model's (SidebarTree.hpp); every extent, ink and radius is the row's tokens (chromeStyle.sidebar,
// chromeStyle.colors), so one file draws the source list, the navigation pane, the header pane and the cell
// tree alike. On the Terminal row the tree's connectors are text (the treeText role) and the meter is block
// characters (meterText), so every part stays on the cell grid.
//
// A click shows what the row names; a double click on a workspace or tab row renames it inline, as F2 (and,
// for the shown workspace, the RenameWorkspace action) does; a right click opens its menu; a workspace row
// drags to reorder the workspaces, a tab row to move the tab and a pane row to move the pane
// (WindowController.beginSidebarDrag), and every row takes a node drop (SidebarModel.drop).
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import Contour.Terminal

Item {
    id: row
    objectName: "sidebarRow"

    required property int index
    required property int kind
    required property int depth
    required property string title
    required property string reason
    required property string fullReason
    required property string gitText
    required property string timeText
    required property int timeKind
    required property int progress
    required property int glyph
    required property int expansion
    required property bool selected
    required property int selection
    required property string treeText
    required property string treeTextContinued
    required property string meterText
    required property string accessibleName
    required property string accessibleDescription
    required property string program
    required property string folder
    required property string reasonLabel
    required property string reasonCommand
    required property string fullReasonLabel
    required property string fullReasonCommand
    required property string hoverDetail
    required property real progressFraction

    /// The sidebar's model (WindowController.sidebar), which the row's actions go to. Its bindings never
    /// read it: the row compares its roles with the SidebarRows enums, so a row that outlives its model while
    /// the window is torn down reads nothing from a model that is gone.
    required property var sidebar
    /// The window's controller, for the row's drag.
    required property var controller
    /// Whether the keyboard is on this row: the tree's current row while the sidebar holds the chrome focus.
    property bool keyboardFocused: false
    /// Whether the keyboard is in the tree at all: a selection is emphasized only then (macOS), unemphasized
    /// while the terminal has it, on the rows whose platform makes the difference (sidebarSelection*Inactive).
    property bool treeFocused: false

    /// The row was hovered long enough for its card, or the pointer left it.
    signal hoverCardWanted(var row, bool wanted)
    /// The tree asks the row to take the keyboard focus (a click on it).
    signal pressed(int index)

    readonly property bool isWorkspace: row.kind === SidebarRows.SidebarRowKind.Workspace
    // A workspace and a tab have names of their own, which the user can change; a pane has none.
    readonly property bool renamable: row.kind !== SidebarRows.SidebarRowKind.Pane
    readonly property bool cellTree: chromeStyle.sidebar.look === chromeStyle.sidebar.cellTree
    readonly property bool hasReason: row.reason !== "" && !row.isWorkspace
    readonly property bool hasProgress: row.timeKind === SidebarRows.TimeKind.Progress && row.progress >= 0
    /// Whether the row wears the selection: the displayed tab, or the folded workspace that holds it.
    readonly property bool marked: row.selection !== SidebarRows.RowSelection.None
    /// How the row wears the selection -- { fill, ring, pill } -- as the look decides it (window::SidebarSelection):
    /// macOS is accent while the tree has the keyboard and grey otherwise, Linux a neutral fill, Fluent a grey
    /// fill with an accent pill, the Terminal inverse video. This item only draws what it is told.
    readonly property var paint: chromeStyle.sidebarSelection(row.selection, row.treeFocused, row.keyboardFocused)
    readonly property bool emphasized: row.paint.fill === "emphasized"
    readonly property color selectionInk: row.emphasized ? chromeStyle.colors.sidebarSelectionInk
                                                         : chromeStyle.colors.sidebarSelectionInkInactive
    readonly property color ink: chromeStyle.paletteRevision < 0 ? "transparent"
                               : row.marked ? row.selectionInk : chromeStyle.colors.tabText
    readonly property color secondaryInk: chromeStyle.paletteRevision < 0 ? "transparent"
                                        : row.marked ? row.selectionInk : chromeStyle.colors.sidebarSecondaryInk
    // The gap between the leading glyphs and the name: at least a cell on the Terminal row, whose small
    // spacing steps are no cells at all, so `▾ billing` never reads `▾billing`.
    readonly property real leadGap: row.cellTree ? Math.max(chromeStyle.space.s3, chromeStyle.cellWidth)
                                                 : chromeStyle.space.s3
    // The selection's and the hover fill's inset: the command palette's on the same row, and none on the
    // Terminal row, whose selection is the whole run of cells.
    readonly property real fillInset: row.cellTree ? 0 : chromeStyle.space.paletteInset
    // A two-line row groups its lines: the reason sits a small step under the name, and the pair is centred in
    // the row, so the rows stand further apart than the lines within one.
    readonly property real lineGap: row.groupsLines ? chromeStyle.space.s1 : 0
    // On the Terminal row each line is a whole cell row, so the first line keeps the row's height there.
    readonly property bool groupsLines: row.hasReason && !row.cellTree
    readonly property real firstLineHeight: row.groupsLines ? Math.max(titleMetrics.height, chromeStyle.sidebar.glyph)
                                                            : chromeStyle.sidebar.rowHeight
    readonly property real firstLineTop: row.groupsLines
        ? Math.max(0, (row.height - row.firstLineHeight - row.lineGap - reasonMetrics.height) / 2) : 0
    // The row's edge padding: 0 on the Terminal row, whose small spacing steps are no cells at all.
    readonly property real padding: chromeStyle.space.s3
    // What the row keeps free at its trailing end, inside the selection: the padding (a cell on the Terminal
    // row, so the times never touch the selection's edge) past the fill's own inset, so a row's trailing text
    // ends the same distance short of the selection as its leading glyph starts past it.
    readonly property real trailPadding: row.cellTree ? Math.max(chromeStyle.space.s3, chromeStyle.cellWidth)
                                                      : chromeStyle.space.s3
    readonly property real trailInset: row.fillInset + row.trailPadding
    /// Whether the inline editor is open.
    readonly property bool renaming: renameLoader.active

    width: ListView.view ? ListView.view.width : implicitWidth
    implicitHeight: row.hasReason ? chromeStyle.sidebar.twoLineRowHeight : chromeStyle.sidebar.rowHeight
    height: implicitHeight

    Accessible.role: Accessible.TreeItem
    Accessible.name: row.accessibleName
    Accessible.description: row.accessibleDescription
    // Qt 6.11's Accessible has no expandable/expanded state to bind: the fold is the description's, in the
    // user's language (accessibleDescriptionOf()).
    Accessible.selectable: true
    Accessible.selected: row.selected
    Accessible.focusable: true
    Accessible.focused: row.keyboardFocused
    Accessible.onPressAction: row.sidebar.activate(row.index)

    /// Opens the inline editor on the row's name: a workspace's label or a tab's title.
    function startRename() {
        if (row.renamable)
            renameLoader.active = true
    }

    FontMetrics { id: titleMetrics; font: titleText.font }
    FontMetrics { id: reasonMetrics; font: reasonText.font }

    // What the pointer does to a row on the rows whose platform shows it (chromeStyle.sidebar.rowFeedback):
    // the band's hover fill under it, the pressed fill while it is held. Under the selection, at its inset.
    Rectangle {
        objectName: "sidebarRowFeedback"
        anchors.fill: parent
        anchors.leftMargin: row.fillInset
        anchors.rightMargin: row.fillInset
        visible: chromeStyle.sidebar.rowFeedback && !row.marked && (primaryTap.pressed || hover.hovered)
        radius: chromeStyle.sidebar.selectionRadius
        color: chromeStyle.paletteRevision < 0 ? "transparent"
             : primaryTap.pressed ? chromeStyle.colors.controlPressedFill : chromeStyle.colors.tabHoverFill
    }

    // The selection: inset and rounded as the row's command palette draws its own (space.paletteInset), with
    // the accent pill at its leading edge where the look decides it has one (row.paint).
    Rectangle {
        objectName: "sidebarRowSelection"
        anchors.fill: parent
        anchors.leftMargin: row.fillInset
        anchors.rightMargin: row.fillInset
        visible: row.marked
        radius: chromeStyle.sidebar.selectionRadius
        color: chromeStyle.paletteRevision < 0 ? "transparent"
             : row.emphasized ? chromeStyle.colors.sidebarSelectionFill
             : chromeStyle.colors.sidebarSelectionFillInactive

        Rectangle {
            objectName: "sidebarRowAccentPill"
            visible: chromeStyle.sidebar.accentPillWidth > 0 && row.paint.pill
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: chromeStyle.sidebar.accentPillWidth
            height: chromeStyle.sidebar.glyph
            radius: chromeStyle.sidebar.accentPillWidth
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarAccentPill
        }
    }

    FocusRing {
        shown: row.paint.ring
        hostRadius: chromeStyle.sidebar.selectionRadius
    }

    // The first line: what leads it, the title, and what trails it. Each part takes whole cells on the
    // Terminal row (snapToCells), so the title and the times keep their columns whatever the glyphs measure.
    Item {
        id: firstLine
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: row.firstLineTop
        height: row.firstLineHeight

        // The gap between the name and what trails it: at least a cell on the Terminal row, where the space
        // tokens are zero, so the name and the branch never touch.
        readonly property real titleGap: row.cellTree ? Math.max(chromeStyle.space.s3, chromeStyle.cellWidth)
                                                      : chromeStyle.space.s3
        // What the branch text may take: whatever the whole name, its gap and the roll-up marker leave. The
        // name identifies the row, so the git text elides first and the name only when nothing else is left.
        readonly property real gitRoom: Math.max(0, firstLine.width - row.trailInset - (lead.x + lead.width)
                                                    - row.leadGap - titleText.implicitWidth
                                                    - firstLine.titleGap
                                                    - (rollUp.visible ? rollUp.width + trail.spacing : 0))

        Row {
            id: lead
            anchors.left: parent.left
            anchors.leftMargin: row.padding + (row.cellTree ? 0 : chromeStyle.sidebar.indent * row.depth)
            anchors.verticalCenter: parent.verticalCenter
            spacing: chromeStyle.space.s3

            // The Terminal look's tree: its connectors as cells.
            Text {
                objectName: "sidebarRowTree"
                visible: row.cellTree && row.treeText !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: chromeStyle.snapToCells(implicitWidth)
                textFormat: Text.PlainText
                text: row.treeText
                font: chromeStyle.type.body
                color: row.secondaryInk
                Accessible.ignored: true
            }

            // A workspace's disclosure; a click folds or unfolds it. The row's token says how it is drawn: a
            // chevron icon on the native rows, centred on the title's capitals rather than sat on the font's
            // baseline, or a glyph in one cell on the Terminal row.
            Item {
                id: disclosure
                objectName: "sidebarRowDisclosure"
                readonly property bool folds: row.expansion !== SidebarRows.RowExpansion.None
                readonly property bool expanded: row.expansion === SidebarRows.RowExpansion.Expanded
                readonly property bool chevron: chromeStyle.sidebar.disclosureIcon !== ""
                visible: row.isWorkspace
                anchors.verticalCenter: parent.verticalCenter
                // The title's box is centred on the line; the centre of its capitals is this far below that.
                anchors.verticalCenterOffset: disclosure.chevron
                    ? (titleMetrics.ascent - titleMetrics.descent - titleMetrics.tightBoundingRect("H").height) / 2
                    : 0
                width: chromeStyle.sidebar.glyph
                height: disclosure.chevron ? chromeStyle.sidebar.glyph : glyphText.implicitHeight

                Text {
                    id: glyphText
                    visible: !disclosure.chevron
                    anchors.fill: parent
                    horizontalAlignment: Text.AlignHCenter
                    textFormat: Text.PlainText
                    text: !disclosure.folds ? "" : disclosure.expanded ? chromeStyle.menuGlyph : chromeStyle.submenuGlyph
                    font: chromeStyle.type.body
                    color: row.secondaryInk
                    Accessible.ignored: true
                }

                IconImage {
                    objectName: "sidebarRowChevron"
                    visible: disclosure.chevron && disclosure.folds
                    anchors.centerIn: parent
                    width: chromeStyle.sidebar.glyph
                    height: chromeStyle.sidebar.glyph
                    sourceSize: Qt.size(width, height)
                    source: chromeStyle.sidebar.disclosureIcon
                    color: row.secondaryInk
                    // Down when expanded; folded, it points where the row's text runs.
                    rotation: disclosure.expanded ? 0 : Qt.application.layoutDirection === Qt.RightToLeft ? 90 : -90
                    Behavior on rotation {
                        enabled: chromeStyle.motion.animates
                        NumberAnimation { duration: chromeStyle.motion.fast }
                    }
                    Accessible.ignored: true
                }

                TapHandler {
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: row.sidebar.toggle(row.index)
                }
            }

            // A tab's or a pane's status; the spinner is TabBadgeSlot's, still under Reduce Motion.
            TabBadgeSlot {
                id: glyphSlot
                objectName: "sidebarRowGlyph"
                visible: !row.isWorkspace && known
                anchors.verticalCenter: parent.verticalCenter
                width: chromeStyle.snapToCells(implicitWidth)
                kind: row.glyph
                shaped: !row.cellTree
                progress: row.hasProgress ? row.progress : -1
            }

            // A tab with nothing to say keeps the glyph's column, so the titles of a tab list stay aligned.
            Item {
                visible: !row.isWorkspace && !glyphSlot.known
                anchors.verticalCenter: parent.verticalCenter
                width: chromeStyle.snapToCells(glyphSlot.edge)
                height: glyphSlot.implicitHeight
            }
        }

        Text {
            id: titleText
            objectName: "sidebarRowTitle"
            visible: !renameLoader.active
            anchors.left: lead.right
            anchors.leftMargin: row.leadGap
            anchors.right: trail.left
            anchors.rightMargin: firstLine.titleGap
            anchors.verticalCenter: parent.verticalCenter
            textFormat: Text.PlainText
            text: row.title
            font: row.isWorkspace ? chromeStyle.type.bodyStrong : chromeStyle.type.body
            color: row.ink
            elide: Text.ElideRight
            Accessible.ignored: true
        }

        Row {
            id: trail
            anchors.right: parent.right
            anchors.rightMargin: row.trailInset
            anchors.verticalCenter: parent.verticalCenter
            spacing: chromeStyle.sidebar.gitGap

            // A workspace's branch and drift (`feat/billing ↑2`).
            Text {
                objectName: "sidebarRowGit"
                visible: row.isWorkspace && row.gitText !== "" && firstLine.gitRoom > 0
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(chromeStyle.snapToCells(implicitWidth), firstLine.gitRoom)
                horizontalAlignment: Text.AlignRight
                textFormat: Text.PlainText
                text: row.gitText
                font: chromeStyle.type.footnote
                color: row.secondaryInk
                elide: Text.ElideRight
                Accessible.ignored: true
            }

            // A workspace's rolled-up status.
            TabBadgeSlot {
                id: rollUp
                objectName: "sidebarRowRollUp"
                visible: row.isWorkspace && known
                anchors.verticalCenter: parent.verticalCenter
                width: chromeStyle.snapToCells(implicitWidth)
                kind: row.glyph
                shaped: !row.cellTree
            }

            // The time in the state, the time since it ended, or the progress's percent.
            Text {
                objectName: "sidebarRowTime"
                visible: !row.isWorkspace && row.timeText !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: chromeStyle.snapToCells(implicitWidth)
                horizontalAlignment: Text.AlignRight
                textFormat: Text.PlainText
                text: row.timeText
                font: chromeStyle.type.footnote
                color: row.secondaryInk
                Accessible.ignored: true
            }

            // The Terminal look's meter: block characters.
            Text {
                objectName: "sidebarRowMeterCells"
                visible: row.cellTree && row.hasProgress
                anchors.verticalCenter: parent.verticalCenter
                width: chromeStyle.snapToCells(implicitWidth)
                horizontalAlignment: Text.AlignRight
                textFormat: Text.PlainText
                text: row.meterText
                font: chromeStyle.type.footnote
                color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.statusInk(chromeStyle.statusRunning)
                Accessible.ignored: true
            }
        }

        // The inline editor over the title, as a tab's (TabItem.qml): Enter commits, Escape cancels, and
        // leaving it commits.
        Loader {
            id: renameLoader
            active: false
            anchors.left: lead.right
            anchors.leftMargin: row.leadGap
            anchors.right: parent.right
            anchors.rightMargin: row.trailInset
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: TextField {
                objectName: "sidebarRenameField"
                text: row.title
                font: chromeStyle.type.body
                selectByMouse: true
                Accessible.name: row.isWorkspace ? qsTr("Workspace name") : qsTr("Tab title")
                Component.onCompleted: { forceActiveFocus(); selectAll() }
                // Guards the focus-out commit from finalizing an edit Enter or Escape already did.
                property bool finalized: false
                onAccepted: {
                    finalized = true
                    row.sidebar.rename(row.index, text)
                    renameLoader.active = false
                    row.renameEnded()
                }
                Keys.onEscapePressed: {
                    finalized = true
                    renameLoader.active = false
                    row.renameEnded()
                }
                onActiveFocusChanged: {
                    if (!activeFocus && !finalized) {
                        finalized = true
                        row.sidebar.rename(row.index, text)
                        renameLoader.active = false
                    }
                }
            }
        }
    }

    /// The editor closed by Enter or Escape: the tree takes the keyboard back.
    signal renameEnded()

    // The Terminal look's tree on the second line: every line passing the row goes on through it, down to the
    // next row and to the row's own children, so a two-line row never breaks the tree.
    Text {
        objectName: "sidebarRowTreeContinued"
        visible: row.cellTree && row.hasReason && row.treeTextContinued !== ""
        x: lead.x
        anchors.top: reasonLine.top
        textFormat: Text.PlainText
        text: row.treeTextContinued
        font: chromeStyle.type.body
        color: row.secondaryInk
        Accessible.ignored: true
    }

    // The second line: the reason's words in the proportional footnote face, and its command, when it has one,
    // in the monospace footnote face on the same baseline. The command elides; the words keep their room.
    Row {
        id: reasonLine
        objectName: "sidebarRowReasonLine"
        visible: row.hasReason
        // Under the title, which sits in the first line: placed by its x, since only siblings anchor.
        x: titleText.x
        width: Math.max(0, row.width - titleText.x - row.trailInset)
        anchors.top: firstLine.bottom
        anchors.topMargin: row.lineGap

        Text {
            id: reasonText
            objectName: "sidebarRowReason"
            // The words alone when there is no command, and elided like a whole line then.
            width: row.reasonCommand === "" ? reasonLine.width : Math.min(implicitWidth, reasonLine.width)
            textFormat: Text.PlainText
            text: row.reasonLabel
            font: chromeStyle.type.footnote
            color: row.secondaryInk
            elide: Text.ElideRight
            Accessible.ignored: true
        }

        Text {
            objectName: "sidebarRowReasonCommand"
            visible: row.reasonCommand !== ""
            width: Math.max(0, reasonLine.width - reasonText.width)
            textFormat: Text.PlainText
            text: row.reasonCommand
            font: chromeStyle.type.monospaceFootnote
            color: row.secondaryInk
            elide: Text.ElideRight
            Accessible.ignored: true
        }
    }

    // The thin meter under a native row that reports progress.
    Rectangle {
        objectName: "sidebarRowMeter"
        visible: !row.cellTree && row.hasProgress
        x: titleText.x
        width: titleText.width
        anchors.bottom: parent.bottom
        height: chromeStyle.size.progressBar
        radius: chromeStyle.size.progressBar
        color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarDivider

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: parent.width * row.progressFraction
            radius: chromeStyle.size.progressBar
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.statusInk(chromeStyle.statusRunning)
        }
    }

    HoverHandler {
        id: hover
        onHoveredChanged: if (!hover.hovered) row.hoverCardWanted(row, false)
    }

    Timer {
        interval: chromeStyle.motion.tooltipDelay
        running: hover.hovered && !row.isWorkspace && !renameLoader.active
        onTriggered: row.hoverCardWanted(row, true)
    }

    TapHandler {
        id: primaryTap
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: (eventPoint, button) => {
            row.pressed(row.index)
            if (tapCount === 2)
                row.startRename()
            else
                row.sidebar.activate(row.index)
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: (eventPoint) => row.sidebar.openContextMenu(row.index,
                                                               row.mapToItem(null, eventPoint.position.x, eventPoint.position.y))
    }

    // Every row drags: a workspace to reorder it, a tab as a strip tab does, a pane to move it out of its tab.
    // The window starts it (beginSidebarDrag), the OS runs it.
    NodeDragSource {
        id: dragSource
        anchors.fill: parent
        controller: row.controller
        begin: function() { return row.controller && row.controller.beginSidebarDrag
                                   ? row.controller.beginSidebarDrag(row.index) : "" }
    }

    DragHandler {
        objectName: "sidebarRowDragHandler"
        enabled: !renameLoader.active
        target: null
        acceptedButtons: Qt.LeftButton
        onActiveChanged: if (active) dragSource.carry()
    }

    // A node dropped on the row: the band of the row it is in decides where it lands (SidebarDropPolicy) --
    // the gap above or below the row, or into it -- and what it would do is shown while it hovers: a line at
    // the gap, or the row highlighted. One the row refuses shows the refusal, and falls through to the window.
    DropArea {
        id: rowDrop
        objectName: "sidebarRowDrop"
        anchors.fill: parent
        keys: [NodeDrag.mime]

        /// What a drop here now would do, as a SidebarRows.DropIndicator.
        property int indicator: SidebarRows.DropIndicator.Refused

        function refresh(drag) {
            const answer = row.sidebar ? row.sidebar.dropIndicator(row.index, drag.y / row.height,
                                                                   drag.getDataAsString(NodeDrag.mime))
                                       : SidebarRows.DropIndicator.Refused
            // Fail closed: a call the engine could not make answers `undefined`, which is no drop at all.
            rowDrop.indicator = typeof answer === "number" ? answer : SidebarRows.DropIndicator.Refused
            drag.accepted = rowDrop.indicator !== SidebarRows.DropIndicator.Refused
        }

        onEntered: (drag) => rowDrop.refresh(drag)
        onPositionChanged: (drag) => rowDrop.refresh(drag)
        onExited: rowDrop.indicator = SidebarRows.DropIndicator.Refused
        onDropped: (drop) => {
            rowDrop.refresh(drop)
            const takes = rowDrop.indicator !== SidebarRows.DropIndicator.Refused
            rowDrop.indicator = SidebarRows.DropIndicator.Refused
            if (!takes) {
                drop.accepted = false
                return
            }
            row.sidebar.drop(row.index, drop.y / row.height, drop.getDataAsString(NodeDrag.mime))
            drop.accept(Qt.MoveAction)
        }

        readonly property color mark: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.accentColor

        // "Into": the row highlighted.
        Rectangle {
            objectName: "sidebarRowDropHighlight"
            anchors.fill: parent
            anchors.leftMargin: row.fillInset
            anchors.rightMargin: row.fillInset
            visible: rowDrop.containsDrag && rowDrop.indicator === SidebarRows.DropIndicator.Highlight
            radius: chromeStyle.sidebar.selectionRadius
            color: Qt.alpha(rowDrop.mark, chromeStyle.dropFillOpacity)
            border.width: chromeStyle.borderWidth
            border.color: rowDrop.mark
        }

        // A gap: the line it lands at, at the row's upper or lower edge.
        Rectangle {
            objectName: "sidebarRowDropLine"
            readonly property bool above: rowDrop.indicator === SidebarRows.DropIndicator.LineAbove
            visible: rowDrop.containsDrag && (above || rowDrop.indicator === SidebarRows.DropIndicator.LineBelow)
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: row.fillInset
            anchors.rightMargin: row.fillInset
            // As thick as the ring a pane card wears for a drop.
            height: chromeStyle.dropRingWidth
            y: above ? 0 : parent.height - height
            color: rowDrop.mark
        }
    }
}
