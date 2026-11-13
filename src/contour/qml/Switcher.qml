// vim:syntax=qml
// The daemon and workspace switcher: a member of the command palette's family, drawn in the window's scene on
// the palette's frame (PickerPopup). Its top level lists the window's daemon's workspaces, the other daemons
// and the actions; typing finds the workspaces of every listed local daemon; a daemon row drills into that
// daemon's level, and "New Daemon…" into the label prompt. Opened by ShowSwitcher, NewDaemon or a switch's
// pick it is centred like the palette; opened by the workspace chip it hangs below the chip.
//
// It decides nothing: the rows, their groups, what each says, the footer, what a key does and what choosing a
// row does are the switcher controller's (window/SwitcherController.hpp), presented here by the window it is
// over (WindowController.switcher). Every size, colour and glyph comes from chromeStyle.
import QtQuick
import QtQuick.Controls

PickerPopup {
    id: root

    // The WindowController. Null-guarded everywhere: it is destroyed before the QML tree on window close.
    required property var controller

    // The switcher shown: taken as the window presents it, and kept until the popup has closed. The window
    // lets go of it as it is withdrawn, while the exit transition still draws its rows.
    property var switcher: null
    // Whether the rows' status glyphs are cells (the Terminal row) rather than shapes.
    readonly property bool cellTree: chromeStyle.sidebar.look === chromeStyle.sidebar.cellTree

    namePrefix: "switcher"
    model: root.switcher
    ownsSelection: false
    levelTitle: root.switcher ? root.switcher.levelTitle : ""
    hintText: root.switcher ? root.switcher.hintText : ""
    emptyText: root.switcher ? root.switcher.emptyText : ""
    footerDescription: root.switcher ? root.switcher.footerDescription : ""
    footerHints: root.switcher ? root.switcher.footerHints : ""
    footerHintSets: root.switcher ? root.switcher.footerHintSets : []
    fieldName: root.switcher ? root.switcher.fieldName : ""
    body: root.switcher && root.switcher.isAskingLabel ? labelBody : null

    // The row under a pointer that moved, or -1: it wears the hover fill and its button, and the selection
    // stays where the keys put it (board 6; unlike the palette, hover does not select).
    property int hoveredIndex: -1
    // The row the open row menu was built for, or -1.
    property int _menuRow: -1
    // Whether the popup closes for a row menu pick, which runs once it has closed: no dismissal then.
    property bool _closingForPick: false

    onPointerMoved: (index) => { root.hoveredIndex = index; }

    /// Opens the menu of row @p index below the pointer, or at the row for a key.
    function openRowMenu(index, atPointer) {
        if (!root.switcher)
            return;
        const entries = root.switcher.rowMenu(index);
        if (entries.length === 0)
            return;
        root._menuRow = index;
        rowMenu.entries = entries;
        if (atPointer) {
            rowMenu.popup();
            return;
        }
        const item = root.list.itemAtIndex(index);
        if (item)
            rowMenu.popup(item, root.textInset, item.height);
        else
            rowMenu.popup();
    }

    /// Shows the switcher, centred, or hanging below @p anchor (a rect in the window's coordinates).
    function present(anchor) {
        root.switcher = root.controller ? root.controller.switcher : null;
        root.anchorRect = anchor ? anchor : null;
        if (!root.opened)
            root.open();
        root.syncFromSwitcher();
    }

    // The field and the selection follow the controller, whoever moved them.
    function syncFromSwitcher() {
        if (!root.switcher)
            return;
        if (root.field.text !== root.switcher.filter)
            root.field.text = root.switcher.filter;
        root.select(root.switcher.currentIndex);
    }

    onOpening: {
        // Reopened during a row menu pick's exit, which Qt then cancels without closed(): that pick is dropped
        // (PickerPopup's onAboutToShow), and this close is a dismissal again.
        root._closingForPick = false;
        root.hoveredIndex = -1;
        Qt.callLater(root.syncFromSwitcher);
    }

    // However it closed, the window lets go of the switcher: after a choice or Escape it has already, and a
    // click outside is a dismissal it cancels. A row menu's pick runs from onPicked, once the keyboard is back.
    onClosed: {
        if (root.controller && !root._closingForPick)
            root.controller.closeSwitcher();
        if (!root._closingForPick)
            root.switcher = null;
    }

    onPicked: (payload) => {
        root._closingForPick = false;
        const switcher = root.switcher;
        root.switcher = null;
        if (switcher)
            switcher.triggerRowMenu(payload.row, payload.actionId);
    }

    onQueryEdited: (text) => {
        if (root.switcher && root.switcher.filter !== text)
            root.switcher.filter = text;
    }

    onKeyPressed: (event) => {
        if (root.switcher && root.switcher.handleKey(event.key, event.modifiers, root.field.cursorPosition))
            event.accepted = true;
    }

    Connections {
        target: root.switcher
        function onCurrentIndexChanged() { root.select(root.switcher.currentIndex); }
        function onFilterChanged() {
            if (root.field.text !== root.switcher.filter)
                root.field.text = root.switcher.filter;
        }
        function onModelReset() {
            root.hoveredIndex = -1;
            root.select(root.switcher.currentIndex);
        }
        function onRowMenuRequested(row) { root.openRowMenu(row, false); }
    }

    Connections {
        target: root.controller
        // A stand-in controller in a test may not present a switcher at all.
        ignoreUnknownSignals: true
        function onSwitcherRequested(anchor) { root.present(anchor); }
        function onSwitcherWithdrawn() { root.close(); }
    }

    delegate: Item {
        id: row

        required property int index
        required property string title
        required property var titleMatches
        required property string groupTitle
        required property int kind
        required property int glyph
        required property string detailText
        required property string detailCommand
        required property string detailTail
        required property string pillText
        required property int pillKind
        required property string accessibleDescription
        required property bool drillsIn
        required property bool opensInNewWindow
        required property string newWindowName

        readonly property bool isCurrent: row.index === root.list.currentIndex
        readonly property bool isHovered: row.index === root.hoveredIndex

        width: ListView.view.width
        height: header.height + entry.height
        readonly property real headerHeight: header.height

        PickerGroupHeader {
            id: header
            width: parent.width
            namePrefix: root.namePrefix
            title: row.groupTitle
            textInset: root.textInset
        }

        Item {
            id: entry
            objectName: "switcherRow"
            anchors.top: header.bottom
            width: parent.width
            height: chromeStyle.size.paletteRow

            Accessible.role: Accessible.ListItem
            Accessible.name: row.title
            Accessible.description: row.accessibleDescription
            Accessible.selected: row.isCurrent

            // The hover fill, under a pointer that moved, lighter than the selection, which draws over it. The
            // Terminal row has none: a cell row's selection alone moves.
            Rectangle {
                objectName: "switcherHover"
                anchors.fill: parent
                anchors.leftMargin: chromeStyle.space.paletteInset
                anchors.rightMargin: chromeStyle.space.paletteInset
                radius: chromeStyle.radii.control
                visible: row.isHovered && !row.isCurrent
                color: chromeStyle.colors.paletteHoverFill
            }

            PickerRowSelection {
                id: selection
                anchors.fill: parent
                anchors.leftMargin: chromeStyle.space.paletteInset
                anchors.rightMargin: chromeStyle.space.paletteInset
                namePrefix: root.namePrefix
                selected: row.isCurrent
            }

            Row {
                id: lead
                x: root.textInset
                height: parent.height
                spacing: chromeStyle.space.s3

                // The status glyph the sidebar shows; its column is kept when there is none, so titles align.
                TabBadgeSlot {
                    id: badge
                    objectName: "switcherRowGlyph"
                    anchors.verticalCenter: parent.verticalCenter
                    width: chromeStyle.snapToCells(implicitWidth)
                    kind: row.glyph
                    shaped: !root.cellTree
                    Accessible.ignored: true
                }

                MatchedLabel {
                    id: titleLabel
                    objectName: "switcherTitle"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, Math.min(implicitWidth, trail.x - chromeStyle.space.s4 - lead.x - x))
                    plainText: row.title
                    matches: row.titleMatches
                    horizontalAlignment: Text.AlignLeft
                    elide: Text.ElideRight
                    font: chromeStyle.type.body
                    color: selection.ink
                }
            }

            Row {
                id: trail
                x: parent.width - width - root.textInset
                height: parent.height
                spacing: chromeStyle.space.s3

                // The detail: words, then the reason's command in monospace, then the time.
                Row {
                    objectName: "switcherDetail"
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 0
                    Text {
                        objectName: "switcherDetailText"
                        textFormat: Text.PlainText
                        Accessible.ignored: true
                        text: row.detailText
                        visible: text !== ""
                        font: chromeStyle.type.footnote
                        color: selection.secondaryInk
                    }
                    Text {
                        objectName: "switcherDetailCommand"
                        textFormat: Text.PlainText
                        Accessible.ignored: true
                        text: row.detailCommand
                        visible: text !== ""
                        font: chromeStyle.type.monospaceFootnote
                        color: selection.secondaryInk
                    }
                    Text {
                        objectName: "switcherDetailTail"
                        textFormat: Text.PlainText
                        Accessible.ignored: true
                        text: row.detailTail
                        visible: text !== ""
                        font: chromeStyle.type.footnote
                        color: selection.secondaryInk
                    }
                }

                // The pill: Current, Other window, the daemon a match is on, or a host's transport.
                Rectangle {
                    objectName: "switcherPill"
                    visible: row.pillText !== ""
                    anchors.verticalCenter: parent.verticalCenter
                    width: pillLabel.implicitWidth + chromeStyle.space.s4
                    height: pillLabel.implicitHeight + chromeStyle.space.s1
                    radius: chromeStyle.radii.hostChip
                    color: chromeStyle.colors.chipFill
                    Accessible.ignored: true
                    Text {
                        id: pillLabel
                        textFormat: Text.PlainText
                        anchors.centerIn: parent
                        text: row.pillText
                        font: chromeStyle.type.footnote
                        color: chromeStyle.colors.chipText
                    }
                }

                // Opens the row's workspace in a new window: on the selected and the hovered row.
                ToolbarButton {
                    id: newWindowButton
                    objectName: "switcherNewWindowButton"
                    readonly property var face: chromeStyle.pickerAccessory
                    visible: row.opensInNewWindow && (row.isCurrent || row.isHovered)
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: newWindowButton.face.glyph ?? ""
                    iconName: newWindowButton.face.iconName ?? ""
                    iconSource: newWindowButton.face.bundledIcon ?? ""
                    glyphColor: selection.secondaryInk
                    accessibleName: row.newWindowName
                    onClicked: if (root.switcher) root.switcher.activateInNewWindow(row.index)
                }

                Label {
                    objectName: "switcherDrillIn"
                    visible: row.drillsIn
                    anchors.verticalCenter: parent.verticalCenter
                    text: chromeStyle.drillInGlyph
                    font: chromeStyle.type.body
                    color: selection.secondaryInk
                    Accessible.ignored: true
                }
            }

            // The secondary button opens the row's menu where the pointer is.
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: root.openRowMenu(row.index, true)
            }

            // A click chooses on release inside the row: a drag out of it chooses nothing.
            TapHandler {
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: {
                    if (!root.switcher)
                        return;
                    root.switcher.currentIndex = row.index;
                    root.switcher.activate(row.index);
                }
            }
        }
    }

    ActionContextMenu {
        id: rowMenu
        objectName: "switcherRowMenu"
        parent: root.contentItem
        // Run once the popup has closed and handed the keyboard back: Rename… focuses the sidebar's editor,
        // which the hand-back would otherwise take straight back.
        onPicked: (actionId) => {
            if (!root.switcher || root._menuRow < 0)
                return;
            root._closingForPick = true;
            root.closeThen({ row: root._menuRow, actionId: actionId });
        }
    }

    // The New Daemon level's body: what a label is, the verdict on the one typed, and the command that does
    // the same from a shell.
    Component {
        id: labelBody
        Column {
            objectName: "switcherLabelBody"
            spacing: chromeStyle.space.s2
            topPadding: chromeStyle.space.s3
            bottomPadding: chromeStyle.space.s3
            leftPadding: root.textInset
            rightPadding: root.textInset

            Text {
                objectName: "switcherLabelCaption"
                width: parent.width - parent.leftPadding - parent.rightPadding
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                text: root.switcher ? root.switcher.labelCaption : ""
                font: chromeStyle.type.footnote
                color: chromeStyle.colors.paletteSecondaryText
            }

            Row {
                spacing: chromeStyle.space.s2
                visible: verdictText.text !== ""
                TabBadgeSlot {
                    objectName: "switcherLabelVerdictGlyph"
                    anchors.verticalCenter: parent.verticalCenter
                    kind: root.switcher ? root.switcher.labelVerdictGlyph : 0
                    shaped: !root.cellTree
                    Accessible.ignored: true
                }
                Text {
                    id: verdictText
                    objectName: "switcherLabelVerdict"
                    textFormat: Text.PlainText
                    text: root.switcher ? root.switcher.labelMessage : ""
                    font: chromeStyle.type.footnote
                    color: chromeStyle.colors.paletteText
                }
            }

            Column {
                visible: commandText.text !== ""
                spacing: chromeStyle.space.s1
                Text {
                    objectName: "switcherLabelCommandCaption"
                    textFormat: Text.PlainText
                    text: root.switcher ? root.switcher.labelCommandCaption : ""
                    font: chromeStyle.type.footnote
                    color: chromeStyle.colors.paletteSecondaryText
                }
                Text {
                    id: commandText
                    objectName: "switcherLabelCommand"
                    textFormat: Text.PlainText
                    text: root.switcher ? root.switcher.labelCommand : ""
                    font: chromeStyle.type.monospaceFootnote
                    color: chromeStyle.colors.paletteText
                }
            }
        }
    }
}
