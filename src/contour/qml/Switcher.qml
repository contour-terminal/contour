// vim:syntax=qml
// The daemon and workspace switcher popup: a filterable list of the daemons -- grouped This computer and
// Saved hosts -- each expanding to its workspaces with their tab count and state, and "+ new workspace on
// X"; or, for a pick a switch asked for, the one daemon's workspaces. "New daemon…" turns the field into a
// label prompt.
//
// The rows, their order, their names and what choosing one does all come from the switcher controller
// (window/SwitcherController.hpp), so this file decides none of them. The keyboard does the work: the field
// keeps focus, the arrows move the selection, Right expands a daemon and Left collapses it (with the caret
// at the field's end or start, so editing the text still moves it), Return switches this window,
// Shift+Return opens a new one, Escape cancels and changes nothing.
//
// It follows the Connect picker's visual language: every size, colour and glyph comes from chromeStyle.
import QtQml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ThemedWindow {
    id: root
    objectName: "switcherWindow"

    // The SwitcherController. Null-guarded everywhere: it may be destroyed before the QML tree.
    required property var switcher

    // Where a row's text starts: the selection's inset from the list, then the same again inside it.
    readonly property real textInset: chromeStyle.space.paletteInset + chromeStyle.space.paletteInset
    readonly property bool isAskingLabel: root.switcher !== null && root.switcher.isAskingLabel

    title: qsTr("Switch")
    visible: false
    width: chromeStyle.size.connectWidth
    height: chromeStyle.size.connectHeight
    minimumWidth: chromeStyle.size.connectWidth
    minimumHeight: chromeStyle.size.connectHeight

    /// Puts the caret in the field the popup shows now.
    function focusField() {
        if (root.isAskingLabel) {
            labelField.forceActiveFocus();
            labelField.selectAll();
        } else {
            filterField.forceActiveFocus();
            filterField.selectAll();
        }
    }

    // Selects @p index and keeps it in view.
    function select(index) {
        list.currentIndex = index;
        if (index >= 0)
            list.positionViewAtIndex(index, ListView.Contain);
    }

    // Closed by the window manager: a start still running must not open a window afterwards.
    onClosing: if (root.switcher) root.switcher.abandon()

    // Where the pointer was at its last event in the list; NaN until one: the first only records it. A popup
    // opening under a pointer at rest, rows scrolled or recreated under it, are no hover and must not take the
    // selection from the keys or from the popup's own choice; only a pointer whose position changed does.
    property real lastPointerX: NaN
    property real lastPointerY: NaN

    // Focus goes to the field whenever the window is shown again.
    onVisibleChanged: if (visible) {
        root.lastPointerX = NaN
        root.lastPointerY = NaN
        root.focusField()
    }
    onIsAskingLabelChanged: {
        labelField.text = "";
        root.focusField();
    }

    Connections {
        target: root.switcher
        function onCurrentIndexChanged() {
            root.select(root.switcher.currentIndex);
        }
        function onFilterChanged() {
            if (filterField.text !== root.switcher.filter)
                filterField.text = root.switcher.filter;
        }
    }

    // {{{ Browse: the daemons and their workspaces
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: !root.isAskingLabel

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: chromeStyle.size.paletteField

            TextField {
                id: filterField
                objectName: "switcherFilter"
                x: root.textInset
                width: parent.width - x - root.textInset
                anchors.verticalCenter: parent.verticalCenter
                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("Search daemons and workspaces")
                placeholderText: qsTr("Search daemons and workspaces")
                font: chromeStyle.type.paletteInput
                color: chromeStyle.colors.paletteText
                placeholderTextColor: chromeStyle.colors.paletteSecondaryText
                background: Item {}
                padding: 0
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase

                onTextChanged: if (root.switcher) root.switcher.filter = text

                Keys.onDownPressed: root.switcher.step(1)
                Keys.onUpPressed: root.switcher.step(-1)
                Keys.onReturnPressed: (event) => {
                    if (event.modifiers & Qt.ShiftModifier)
                        root.switcher.openCurrent();
                    else
                        root.switcher.activateCurrent();
                }
                Keys.onEnterPressed: (event) => {
                    if (event.modifiers & Qt.ShiftModifier)
                        root.switcher.openCurrent();
                    else
                        root.switcher.activateCurrent();
                }
                Keys.onEscapePressed: root.switcher.cancel()
                Keys.onRightPressed: (event) => {
                    if (cursorPosition === length)
                        root.switcher.expandCurrent();
                    else
                        event.accepted = false;
                }
                Keys.onLeftPressed: (event) => {
                    if (cursorPosition === 0)
                        root.switcher.collapseCurrent();
                    else
                        event.accepted = false;
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: chromeStyle.borderWidth
            color: chromeStyle.colors.paletteSeparator
        }

        ListView {
            id: list
            objectName: "switcherList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.switcher
            currentIndex: root.switcher ? root.switcher.currentIndex : -1
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar {}

            // Moving the pointer over a row selects it. One handler for the list rather than one per row: Qt
            // re-delivers hover to rows that appear or move under a pointer at rest, and a row cannot tell
            // that from the pointer arriving; the position can.
            HoverHandler {
                onPointChanged: {
                    const at = point.scenePosition
                    const moved = !isNaN(root.lastPointerX)
                                  && (at.x !== root.lastPointerX || at.y !== root.lastPointerY)
                    root.lastPointerX = at.x
                    root.lastPointerY = at.y
                    if (!moved || !hovered || !root.switcher)
                        return
                    const inList = list.mapFromItem(null, at.x, at.y)
                    const index = list.indexAt(inList.x + list.contentX, inList.y + list.contentY)
                    if (index >= 0)
                        root.switcher.currentIndex = index
                }
            }

            delegate: Item {
                id: row

                required property int index

                required property string title
                required property string detail
                required property string tag
                required property string groupTitle
                required property string kind
                required property bool expanded

                readonly property bool isCurrent: row.index === list.currentIndex
                // Workspaces and the new-workspace row sit under their daemon.
                readonly property real indent: row.kind === "workspace" || row.kind === "new-workspace"
                                               ? chromeStyle.space.s8 : 0

                width: ListView.view.width
                height: header.height + entry.height

                Item {
                    id: header
                    objectName: "switcherGroupHeader"
                    width: parent.width
                    height: visible ? chromeStyle.size.paletteGroupHeader : 0
                    visible: row.groupTitle !== ""

                    Accessible.role: Accessible.Heading
                    Accessible.name: row.groupTitle

                    Label {
                        x: root.textInset
                        width: parent.width - x - root.textInset
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.groupTitle
                        font: chromeStyle.type.footnoteSemibold
                        elide: Text.ElideRight
                        color: chromeStyle.colors.paletteSecondaryText
                        Accessible.ignored: true
                    }
                }

                Item {
                    id: entry
                    objectName: "switcherRow"
                    anchors.top: header.bottom
                    width: parent.width
                    height: chromeStyle.size.connectRow

                    property string rowKind: row.kind

                    Accessible.role: Accessible.ListItem
                    Accessible.name: row.title
                    Accessible.description: row.detail

                    readonly property color textColor: row.isCurrent ? chromeStyle.colors.paletteSelectionText
                                                                     : chromeStyle.colors.paletteText
                    readonly property color secondaryColor: row.isCurrent
                                                            ? chromeStyle.colors.paletteSelectionText
                                                            : chromeStyle.colors.paletteSecondaryText

                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: chromeStyle.space.paletteInset
                        anchors.rightMargin: chromeStyle.space.paletteInset
                        radius: chromeStyle.radii.control
                        visible: row.isCurrent
                        color: chromeStyle.colors.paletteSelectionFill
                    }

                    // A daemon's disclosure: ▸ collapsed, ▾ expanded; a tap toggles it.
                    Label {
                        id: disclosure
                        objectName: "switcherDisclosure"
                        x: root.textInset
                        width: chromeStyle.space.s8
                        anchors.verticalCenter: parent.verticalCenter
                        visible: row.kind === "daemon"
                        text: row.expanded ? "▾" : "▸"
                        font: chromeStyle.type.body
                        color: entry.secondaryColor
                        Accessible.ignored: true

                        TapHandler {
                            onTapped: if (root.switcher) root.switcher.toggle(row.index)
                        }
                    }

                    Column {
                        x: root.textInset + chromeStyle.space.s8 + row.indent
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, accessory.x - chromeStyle.space.s4 - x)

                        Label {
                            objectName: "switcherTitle"
                            width: parent.width
                            text: row.title
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            font: chromeStyle.type.body
                            color: entry.textColor
                        }
                        Label {
                            objectName: "switcherDetail"
                            width: parent.width
                            visible: row.detail !== ""
                            text: row.detail
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            font: chromeStyle.type.footnote
                            color: entry.secondaryColor
                        }
                    }

                    Rectangle {
                        id: accessory
                        objectName: "switcherTag"
                        visible: row.tag !== ""
                        x: parent.width - width - root.textInset
                        anchors.verticalCenter: parent.verticalCenter
                        width: visible ? tagLabel.implicitWidth + chromeStyle.space.s4 + chromeStyle.space.s4 : 0
                        height: tagLabel.implicitHeight + chromeStyle.space.s1 + chromeStyle.space.s1
                        radius: chromeStyle.radii.toolbarChip
                        color: chromeStyle.colors.chipFill

                        Label {
                            id: tagLabel
                            anchors.centerIn: parent
                            text: row.tag
                            font: chromeStyle.type.caption
                            color: chromeStyle.colors.chipText
                            Accessible.ignored: true
                        }
                    }

                    // A tap chooses the row; the list's HoverHandler selects it as the pointer moves over it.
                    TapHandler {
                        onTapped: {
                            root.switcher.currentIndex = row.index;
                            root.switcher.activate(row.index);
                        }
                    }
                }
            }
        }

        Label {
            objectName: "switcherEmpty"
            visible: list.count === 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? chromeStyle.size.paletteRow : 0
            leftPadding: root.textInset
            verticalAlignment: Text.AlignVCenter
            text: qsTr("No matching daemons or workspaces")
            font: chromeStyle.type.body
            color: chromeStyle.colors.paletteSecondaryText
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: chromeStyle.borderWidth
            color: chromeStyle.colors.paletteSeparator
        }

        Label {
            objectName: "switcherKeys"
            Layout.fillWidth: true
            Layout.leftMargin: root.textInset
            Layout.rightMargin: root.textInset
            Layout.topMargin: chromeStyle.space.s4
            Layout.bottomMargin: chromeStyle.space.s4
            text: qsTr("Return = Switch here · Shift+Return = Open in new window · Esc = cancel")
            elide: Text.ElideRight
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.paletteSecondaryText
        }
    }
    // }}}

    // {{{ New daemon…: the label prompt
    ColumnLayout {
        objectName: "switcherLabelPrompt"
        anchors.fill: parent
        anchors.leftMargin: root.textInset
        anchors.rightMargin: root.textInset
        anchors.topMargin: chromeStyle.space.s4
        spacing: chromeStyle.space.s4
        visible: root.isAskingLabel

        Label {
            Layout.fillWidth: true
            text: qsTr("New daemon")
            font: chromeStyle.type.footnoteSemibold
            color: chromeStyle.colors.paletteSecondaryText
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }

        TextField {
            id: labelField
            objectName: "switcherLabelField"
            Layout.fillWidth: true
            Accessible.role: Accessible.EditableText
            Accessible.name: qsTr("Label of the new daemon")
            placeholderText: qsTr("Label, such as work2")
            font: chromeStyle.type.paletteInput
            color: chromeStyle.colors.paletteText
            placeholderTextColor: chromeStyle.colors.paletteSecondaryText
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            enabled: root.switcher !== null && !root.switcher.isBusy

            onTextChanged: if (root.switcher) root.switcher.checkLabel(text)
            Keys.onReturnPressed: root.switcher.submitLabel(text)
            Keys.onEnterPressed: root.switcher.submitLabel(text)
            Keys.onEscapePressed: root.switcher.cancel()
        }

        Label {
            objectName: "switcherLabelError"
            Layout.fillWidth: true
            visible: root.switcher !== null && root.switcher.labelError !== ""
            text: root.switcher ? root.switcher.labelError : ""
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            font: chromeStyle.type.footnote
            color: chromeStyle.statusInk(chromeStyle.statusDanger)
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }

        Label {
            objectName: "switcherLabelBusy"
            Layout.fillWidth: true
            visible: root.switcher !== null && root.switcher.isBusy
            text: qsTr("Starting the daemon…")
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.paletteSecondaryText
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.switcher !== null && root.switcher.runningLabel !== ""
            spacing: chromeStyle.space.s4

            Label {
                Layout.fillWidth: true
                text: root.switcher ? qsTr("Daemon \"%1\" is already running.").arg(root.switcher.runningLabel)
                                    : ""
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font: chromeStyle.type.footnote
                color: chromeStyle.colors.paletteText
            }

            Button {
                objectName: "switcherSwitchOffer"
                text: root.switcher ? qsTr("Switch to daemon %1").arg(root.switcher.runningLabel) : ""
                font: chromeStyle.type.footnote
                highlighted: true
                onClicked: root.switcher.switchToRunning()
            }
        }

        Item {
            Layout.fillHeight: true
        }
    }
    // }}}
}
