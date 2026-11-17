// vim:syntax=qml
// One row of the workspace sidebar (SidebarModel): a workspace, a tab of one, or a pane of a split tab.
//
// A workspace row: its disclosure, its name, its git state and the rolled-up status glyph. A tab or pane row:
// its status glyph, its title, a second line -- its reason when there is one, else its folder and program --
// and its time -- or its progress, as a thin meter -- at the right. A tab or pane row is always two lines
// tall. Everything the row says, and in what order the statuses rank, is the model's (SidebarTree.hpp);
// every extent, ink and radius is the row's tokens (chromeStyle.sidebar, chromeStyle.colors), so one file
// draws the source list, the navigation pane, the header pane and the cell tree alike. On the Terminal row
// the tree's connectors are text (the treeText role) and the meter is block characters (meterText), so every
// part stays on the cell grid.
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
    required property int secondLine
    required property string idleLine
    required property color tabColor
    /// The colour of the workspace the row belongs to (SidebarModel's workspaceColor role); transparent when
    /// it has none.
    required property color workspaceColor
    /// What the Terminal look's workspace rail draws on the row's first line (window::workspaceRailCellsOf()):
    /// the tree's first cell, made to run on; empty on a workspace row whose disclosure is that cell.
    required property string railText
    /// What the rail draws on the row's second line; empty on a workspace row.
    required property string railTextContinued
    required property var icon

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
    readonly property bool hasReason: row.secondLine === SidebarRows.SecondLine.Status
    readonly property bool hasProgress: row.timeKind === SidebarRows.TimeKind.Progress && row.progress >= 0
    /// Whether the row wears the selection: the displayed tab, or the folded workspace that holds it.
    readonly property bool marked: row.selection !== SidebarRows.RowSelection.None
    /// How the row wears the selection -- { fill, ring, pill } -- as the look decides it (window::SidebarSelection):
    /// macOS is accent while the tree has the keyboard and grey otherwise, Linux a neutral fill, Fluent a grey
    /// fill with an accent pill, the Terminal inverse video. This item only draws what it is told.
    readonly property var paint: chromeStyle.sidebarSelection(row.selection, row.treeFocused, row.keyboardFocused)
    readonly property bool emphasized: row.paint.fill === "emphasized"
    /// What the pointer does to the row (window::RowPointer): a coloured row's tint fades by it, and keeps
    /// more of its colour while pressed than while hovered.
    readonly property int pointer: chromeStyle.sidebarRowPointer(hover.hovered, primaryTap.pressed)
    /// What the tab's colour paints on the row -- { fill, bar, ink }, each transparent where it paints nothing
    /// -- in the configured style (`sidebar.tab_color`, window::SidebarTabColor), faded by the row's state as
    /// the tab strip fades a coloured tab. This item only draws what it is told.
    readonly property var tabPaint: chromeStyle.sidebarTabColor(row.controller ? row.controller.sidebarTabColorStyle : 0,
                                                                row.tabColor, chromeStyle.colors.sidebarFill,
                                                                row.selection, row.pointer, Window.active)
    /// Whether the tab's colour fills the row: it then stands in for the selection's and the hover's fill.
    readonly property bool tinted: row.tabPaint.fill.a > 0
    readonly property bool hasTabBar: row.tabPaint.bar.a > 0
    readonly property bool hasTabInk: row.tabPaint.ink.a > 0
    /// What the workspace's colour paints on the row -- { fill, ink, rail, wash, swatch }, each transparent
    /// where it paints nothing -- in the configured style (`sidebar.workspace_color`,
    /// window::SidebarWorkspaceColor). This item only draws what it is told.
    readonly property var workspacePaint: chromeStyle.sidebarWorkspaceColor(
        row.controller ? row.controller.sidebarWorkspaceColorStyle : 0, row.kind, row.workspaceColor,
        chromeStyle.colors.sidebarFill, row.selection, row.pointer, Window.active)
    /// Whether the workspace's colour fills the row (its own row, `tint`): it then stands in for the
    /// selection's and the hover's fill, as a tab's tint does.
    readonly property bool workspaceTinted: row.workspacePaint.fill.a > 0
    readonly property bool hasWorkspaceRail: row.workspacePaint.rail.a > 0
    readonly property bool hasWorkspaceWash: row.workspacePaint.wash.a > 0
    readonly property bool hasWorkspaceSwatch: row.workspacePaint.swatch.a > 0
    /// On the Terminal row the rail is the first cell of the tree's connectors, drawn in the colour.
    readonly property bool railCell: row.cellTree && row.hasWorkspaceRail && !row.isWorkspace
                                     && row.treeText !== ""
    /// The tint the row's text and marks sit on, { fill, ink }: the tab's, else the workspace's.
    readonly property var tint: row.tinted ? row.tabPaint : row.workspacePaint
    /// Whether a colour fills the row, the tab's or the workspace's.
    readonly property bool filled: row.tinted || row.workspaceTinted
    /// The progress meter's ink: the running status's, unless the row's tint would swallow it.
    readonly property color runningInk: chromeStyle.paletteRevision < 0 ? "transparent"
        : chromeStyle.sidebarMarkInk(chromeStyle.statusInk(chromeStyle.statusRunning), row.tint.fill, row.tint.ink)
    readonly property color selectionInk: row.emphasized ? chromeStyle.colors.sidebarSelectionInk
                                                         : chromeStyle.colors.sidebarSelectionInkInactive
    readonly property color ink: chromeStyle.paletteRevision < 0 ? "transparent"
                               : row.marked ? row.selectionInk : chromeStyle.colors.tabText
    // On a tinted row everything the row writes takes the ink chosen for the tint, so it stays legible on it.
    readonly property color secondaryInk: chromeStyle.paletteRevision < 0 ? "transparent"
                                        : row.filled ? row.tint.ink
                                        : row.marked ? row.selectionInk : chromeStyle.colors.sidebarSecondaryInk
    // The gap between the leading glyphs and the name: at least a cell on the Terminal row, whose small
    // spacing steps are no cells at all, so `▾ billing` never reads `▾billing`.
    readonly property real leadGap: row.cellTree ? Math.max(chromeStyle.space.s3, chromeStyle.cellWidth)
                                                 : chromeStyle.space.s3
    // The selection's and the hover fill's inset: the command palette's on the same row, and none on the
    // Terminal row, whose selection is the whole run of cells.
    readonly property real fillInset: row.cellTree ? 0 : chromeStyle.space.paletteInset
    // A tab or pane row is always two lines, the reason or the idle line under the name, so a status coming
    // or going changes text and never the row's height. It groups its lines: the second sits a small step
    // (sidebar.lineGap) under the name, and the pair is centred in the row, so the rows stand further apart
    // than the lines within one.
    readonly property real lineGap: row.groupsLines ? chromeStyle.sidebar.lineGap : 0
    // On the Terminal row each line is a whole cell row, so the first line keeps the row's height there.
    readonly property bool groupsLines: !row.isWorkspace && !row.cellTree
    /// Whether a tab or pane row has anything for its second line (SidebarTree's secondLineOf()). A row that
    /// has not keeps its two-line height and centres its name in it, except on the Terminal row, whose name
    /// keeps the first of its two cell rows, as half a cell is no place on the grid.
    readonly property bool hasSecondLine: row.secondLine !== SidebarRows.SecondLine.None
    readonly property real firstLineHeight: row.groupsLines ? Math.max(titleMetrics.height, chromeStyle.sidebar.glyph)
                                                            : chromeStyle.sidebar.rowHeight
    // Both second lines are set in the footnote face, so one measure centres the pair whichever it is.
    readonly property real linesHeight: row.firstLineHeight
                                        + (row.hasSecondLine ? row.lineGap + secondLineMetrics.height : 0)
    readonly property real firstLineTop: row.groupsLines ? Math.max(0, (row.height - row.linesHeight) / 2) : 0
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
    implicitHeight: row.isWorkspace ? chromeStyle.sidebar.rowHeight : chromeStyle.sidebar.twoLineRowHeight
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
    Accessible.onPressAction: if (row.controller) row.controller.activateSidebarRow(row.index)

    /// Opens the inline editor on the row's name: a workspace's label or a tab's title.
    function startRename() {
        if (row.renamable)
            renameLoader.active = true
    }

    FontMetrics { id: titleMetrics; font: titleText.font }
    FontMetrics { id: secondLineMetrics; font: chromeStyle.type.footnote }

    // A coloured workspace's wash (`sidebar.workspace_color: group`): faint, behind everything the row draws,
    // so a coloured tab's own tint, the selection and the hover draw over it in their inset shape. It is the
    // row's whole box, from the rail at the leading edge to the sidebar's other edge, square, so the rows of one
    // workspace join into one unbroken band (whole cells on the Terminal row, whose rows are whole cells).
    Rectangle {
        objectName: "sidebarRowWorkspaceWash"
        anchors.fill: parent
        visible: row.hasWorkspaceWash
        radius: 0
        color: row.workspacePaint.wash
        Accessible.ignored: true
    }

    // A coloured workspace's tint on its own row (`sidebar.workspace_color: tint`), as a tab's below: in place
    // of the hover's and the selection's fill, faded by the row's state.
    Rectangle {
        objectName: "sidebarRowWorkspaceFill"
        anchors.fill: parent
        anchors.leftMargin: row.fillInset
        anchors.rightMargin: row.fillInset
        visible: row.workspaceTinted
        radius: chromeStyle.sidebar.selectionRadius
        color: row.workspacePaint.fill
        Accessible.ignored: true
    }

    // A coloured tab's tint (`sidebar.tab_color: tint`), in place of the hover's and the selection's fill, with
    // the selection's inset and radius. Its fade tells selected, hovered and resting rows apart.
    Rectangle {
        objectName: "sidebarRowTabFill"
        anchors.fill: parent
        anchors.leftMargin: row.fillInset
        anchors.rightMargin: row.fillInset
        visible: row.tinted
        radius: chromeStyle.sidebar.selectionRadius
        color: row.tabPaint.fill
    }

    // What the pointer does to a row on the rows whose platform shows it (chromeStyle.sidebar.rowFeedback):
    // the band's hover fill under it, the pressed fill while it is held. Under the selection, at its inset.
    Rectangle {
        objectName: "sidebarRowFeedback"
        anchors.fill: parent
        anchors.leftMargin: row.fillInset
        anchors.rightMargin: row.fillInset
        visible: chromeStyle.sidebar.rowFeedback && !row.marked && !row.filled
                 && (primaryTap.pressed || hover.hovered)
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
        visible: row.marked && !row.filled
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

    // A coloured workspace's rail (`sidebar.workspace_color: group`): the row's full height in the sidebar's
    // leading gutter, outside the selection's inset, so the rows of one workspace form one unbroken line. On
    // the Terminal row there is no gutter: the rail is the first cell of the tree's connectors, once on each
    // of the row's lines, drawn in the colour in the room the lead keeps for it (railRoom), as the model
    // says it (railText, railTextContinued); on a workspace row that cell is the disclosure, which takes the
    // colour itself, or, where it folds nothing, the rail's own cell.
    Item {
        id: workspaceRail
        objectName: "sidebarRowWorkspaceRail"
        /// The rail's colour: what the native rows fill it with, and the cell tree inks its cells in.
        readonly property color color: row.workspacePaint.rail
        /// The cell the rail draws on the row's first line of the Terminal row; empty elsewhere.
        readonly property string firstCell: row.cellTree && row.hasWorkspaceRail ? row.railText : ""
        visible: row.hasWorkspaceRail
        x: row.cellTree ? lead.x : 0
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: chromeStyle.sidebar.workspaceRailWidth
        Accessible.ignored: true

        Rectangle {
            visible: !row.cellTree
            anchors.fill: parent
            color: workspaceRail.color
        }

        Column {
            visible: row.cellTree

            Text {
                objectName: "sidebarRowWorkspaceRailCell"
                height: chromeStyle.sidebar.rowHeight
                verticalAlignment: Text.AlignVCenter
                textFormat: Text.PlainText
                text: workspaceRail.firstCell
                font: chromeStyle.type.body
                color: workspaceRail.color
                Accessible.ignored: true
            }
            Text {
                objectName: "sidebarRowWorkspaceRailCellContinued"
                visible: !row.isWorkspace
                height: chromeStyle.sidebar.rowHeight
                verticalAlignment: Text.AlignVCenter
                textFormat: Text.PlainText
                text: row.railTextContinued
                font: chromeStyle.type.body
                color: workspaceRail.color
                Accessible.ignored: true
            }
        }
    }

    // A coloured tab's bar (`sidebar.tab_color: bar`): the row's full height at the leading edge of the
    // selection's fill, inside the workspace's rail. On the Terminal row it is the tab strip's colour cell,
    // once on each of the row's lines, in the room the lead keeps for it past the tree's connectors.
    Item {
        id: tabBar
        objectName: "sidebarRowTabBar"
        /// The bar's colour: what the native rows fill it with, and the cell tree inks its glyphs in.
        readonly property color color: row.tabPaint.bar
        visible: row.hasTabBar
        x: row.cellTree ? lead.x + barRoom.x : row.fillInset
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: chromeStyle.sidebar.tabColorBarWidth

        Rectangle {
            visible: !row.cellTree
            anchors.fill: parent
            color: tabBar.color
        }

        Column {
            visible: row.cellTree

            Repeater {
                model: Math.round(row.height / chromeStyle.sidebar.rowHeight)
                Text {
                    objectName: "sidebarRowTabBarCell"
                    height: chromeStyle.sidebar.rowHeight
                    // Centred in its line, as the tree's connectors and the rail's cells are.
                    verticalAlignment: Text.AlignVCenter
                    textFormat: Text.PlainText
                    text: chromeStyle.tabColorGlyph
                    font: chromeStyle.type.body
                    color: tabBar.color
                    Accessible.ignored: true
                }
            }
        }
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

            // On the Terminal row, the cell the workspace's rail is drawn in (sidebarRowWorkspaceRail): the
            // tree's first cell, which the rail draws in the colour in place of the tree.
            Item {
                id: railRoom
                visible: row.railCell
                width: chromeStyle.sidebar.workspaceRailWidth
                // A positioner lays out no item of zero height: without one, the room would take no cell.
                height: chromeStyle.sidebar.rowHeight
            }

            // The Terminal look's tree: its connectors as cells, past the rail's cell where it has one.
            Text {
                id: treeCells
                objectName: "sidebarRowTree"
                visible: row.cellTree && row.treeText !== "" && !(row.railCell && row.treeText.length === 1)
                anchors.verticalCenter: parent.verticalCenter
                width: chromeStyle.snapToCells(implicitWidth)
                textFormat: Text.PlainText
                text: row.railCell ? row.treeText.substring(1) : row.treeText
                font: chromeStyle.type.body
                color: row.secondaryInk
                Accessible.ignored: true
            }

            // On the Terminal row, the cell the tab colour's bar is drawn in (sidebarRowTabBar), so the bar
            // never covers the tree's connectors.
            Item {
                id: barRoom
                visible: row.cellTree && row.hasTabBar
                width: chromeStyle.sidebar.tabColorBarWidth
                // A positioner lays out no item of zero height: without one, the bar would sit on column 0.
                height: chromeStyle.sidebar.rowHeight
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
                    objectName: "sidebarRowDisclosureGlyph"
                    visible: !disclosure.chevron
                    anchors.fill: parent
                    horizontalAlignment: Text.AlignHCenter
                    textFormat: Text.PlainText
                    text: !disclosure.folds ? "" : disclosure.expanded ? chromeStyle.menuGlyph : chromeStyle.submenuGlyph
                    font: chromeStyle.type.body
                    // The first cell of a workspace row: on the Terminal row the workspace's rail inks it.
                    color: row.cellTree && row.hasWorkspaceRail ? workspaceRail.color : row.secondaryInk
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

            // A coloured workspace's swatch (`sidebar.workspace_color: swatch`), before its name: a square on
            // the native rows, one glyph cell on the Terminal row.
            Item {
                id: workspaceSwatch
                objectName: "sidebarRowWorkspaceSwatch"
                readonly property color color: row.workspacePaint.swatch
                visible: row.hasWorkspaceSwatch
                anchors.verticalCenter: parent.verticalCenter
                // On the Terminal row the glyph's whole cells: a fallback face may draw it wider than one.
                width: row.cellTree ? Math.max(chromeStyle.sidebar.workspaceSwatchSize,
                                               chromeStyle.snapToCells(swatchGlyph.implicitWidth))
                                    : chromeStyle.sidebar.workspaceSwatchSize
                height: row.cellTree ? swatchGlyph.implicitHeight : chromeStyle.sidebar.workspaceSwatchSize
                Accessible.ignored: true

                Rectangle {
                    visible: !row.cellTree
                    anchors.fill: parent
                    radius: chromeStyle.radii.control
                    color: workspaceSwatch.color
                }

                Text {
                    id: swatchGlyph
                    objectName: "sidebarRowWorkspaceSwatchGlyph"
                    visible: row.cellTree
                    // Centred in its cells: a fallback face may draw the square narrower than them.
                    anchors.fill: parent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    textFormat: Text.PlainText
                    text: chromeStyle.workspaceSwatchGlyph
                    font: chromeStyle.type.body
                    color: workspaceSwatch.color
                    Accessible.ignored: true
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
                tint: row.tint
                progress: row.hasProgress ? row.progress : -1
            }

            // A tab with nothing to say keeps the glyph's column, so the titles of a tab list stay aligned.
            Item {
                visible: !row.isWorkspace && !glyphSlot.known
                anchors.verticalCenter: parent.verticalCenter
                width: chromeStyle.snapToCells(glyphSlot.edge)
                height: glyphSlot.implicitHeight
            }

            // A tab's icon as the strip shows it (IconRole): the running program's initial or the profile's
            // image, for the tabs this window holds. The strip's plain square says nothing a row needs, so a
            // tab without one of its own shows none here.
            TabIconMark {
                id: rowIcon
                objectName: "sidebarRowIcon"
                info: row.icon
                fontFamily: chromeStyle.type.body.family
                visible: rowIcon.isMonogram || rowIcon.isImage
                anchors.verticalCenter: parent.verticalCenter
                width: chromeStyle.sidebar.glyph
                height: chromeStyle.sidebar.glyph
                radius: chromeStyle.radii.tabIcon
                color: rowIcon.isImage ? "transparent" : row.selected ? chromeStyle.accentColor : row.secondaryInk
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
            color: row.hasTabInk ? row.tabPaint.ink : row.workspaceTinted ? row.workspacePaint.ink : row.ink
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
                tint: row.tint
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
                color: row.runningInk
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
        visible: row.cellTree && !row.isWorkspace && row.treeTextContinued !== ""
        // Past the rail's cell, which draws the first one in the workspace's colour: under the first line's
        // own connectors, whatever the lead's spacing.
        x: lead.x + (row.railCell ? treeCells.x : 0)
        anchors.top: reasonLine.top
        textFormat: Text.PlainText
        text: row.railCell ? row.treeTextContinued.substring(1) : row.treeTextContinued
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

    // The second line of a row with nothing to say: where it is and what runs there (`~/src · zsh`), in the
    // row's muted ink. A row that knows neither has no second line, and centres its name (firstLineTop).
    Text {
        objectName: "sidebarRowIdle"
        visible: row.secondLine === SidebarRows.SecondLine.Idle
        x: titleText.x
        width: reasonLine.width
        anchors.top: firstLine.bottom
        anchors.topMargin: row.lineGap
        textFormat: Text.PlainText
        text: row.idleLine
        font: chromeStyle.type.footnote
        color: row.secondaryInk
        elide: Text.ElideRight
        Accessible.ignored: true
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
            color: row.runningInk
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
