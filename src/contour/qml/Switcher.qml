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

    // The switcher this window presents, or null while it presents none.
    readonly property var switcher: root.controller && root.controller.switcher ? root.controller.switcher : null
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
    fieldName: qsTr("Switch workspace or daemon")

    /// Shows the switcher, centred, or hanging below @p anchor (a rect in the window's coordinates).
    function present(anchor) {
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

    onOpening: Qt.callLater(root.syncFromSwitcher)

    // However it closed -- a choice, Escape, a click outside -- the window lets go of the switcher.
    onClosed: {
        if (root.controller)
            root.controller.closeSwitcher();
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
        function onModelReset() { root.select(root.switcher.currentIndex); }
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

        readonly property bool isCurrent: row.index === root.list.currentIndex

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
}
