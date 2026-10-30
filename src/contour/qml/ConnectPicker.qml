// vim:syntax=qml
// The Connect picker's content: one question -- where do this window's sessions live? -- as a search field
// over a grouped list, with the footer's "Manage Hosts…", the start-up switch, Cancel and Connect.
//
// The rows, their order, their numbers and the failure shown under a row all come from the picker
// controller (window/ConnectPickerController.hpp), so this file decides none of them. The keyboard does the
// work: the field keeps focus, the arrows move the selection, Return connects, Escape cancels, Alt+1..9
// (Command+1..9 on macOS) connects at once -- plain digits stay typable, for addresses and ports -- and
// Ctrl+L (Command+L) puts the caret back in the field.
//
// Every size, colour and glyph comes from chromeStyle, so the same file draws the native rows in pixels and
// the Terminal row in cells. It hosts itself in a window (ConnectPickerWindow.qml); the sheet of File >
// New Window to… is that window made modal.
import QtQml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    objectName: "connectPicker"

    // The ConnectPickerController. Null-guarded everywhere: it is destroyed before the QML tree on window
    // close, and a binding re-evaluated against null would raise a TypeError the test gate turns into a failure.
    required property var picker

    // Where a row's text starts: the selection's inset from the list, then the same again inside it.
    readonly property real textInset: chromeStyle.space.paletteInset + chromeStyle.space.paletteInset

    /// Puts the caret in the field and selects what is typed there.
    function focusField() {
        filterField.forceActiveFocus();
        filterField.selectAll();
    }

    // Selects @p index and keeps it in view.
    function select(index) {
        list.currentIndex = index;
        if (index >= 0)
            list.positionViewAtIndex(index, ListView.Contain);
    }

    Component.onCompleted: filterField.forceActiveFocus()

    // The controller's selection and the list's are one: the controller moves it (arrows, a new filter, the
    // last used target), the list shows it, and a hover or a tap writes it back.
    Connections {
        target: root.picker
        function onCurrentIndexChanged() {
            root.select(root.picker.currentIndex);
        }
        function onFilterChanged() {
            if (filterField.text !== root.picker.filter)
                filterField.text = root.picker.filter;
        }
    }

    // Alt+1..9 (Command+1..9): the row that digit names, at once. Window-wide, so it works from the field.
    Instantiator {
        model: 9
        delegate: Shortcut {
            required property int index
            context: Qt.WindowShortcut
            enabled: root.picker !== null
            sequence: root.picker ? root.picker.quickModifier + "+" + (index + 1) : ""
            onActivated: root.picker.quickSelect(index + 1)
        }
    }

    // Ctrl+L (Command+L): back to the field.
    Shortcut {
        objectName: "connectFocusField"
        context: Qt.WindowShortcut
        enabled: root.picker !== null
        sequence: root.picker ? root.picker.focusModifier + "+L" : ""
        onActivated: root.focusField()
    }

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // {{{ The field
        Item {
            id: queryRow
            objectName: "connectQuery"
            Layout.fillWidth: true
            Layout.preferredHeight: chromeStyle.size.paletteField

            Label {
                id: queryPrompt
                x: root.textInset
                anchors.verticalCenter: parent.verticalCenter
                visible: chromeStyle.palettePromptGlyph !== ""
                text: chromeStyle.palettePromptGlyph
                font: chromeStyle.type.paletteInput
                color: chromeStyle.colors.paletteSecondaryText
                Accessible.ignored: true
            }

            TextField {
                id: filterField
                objectName: "connectFilter"
                x: queryPrompt.visible ? queryPrompt.x + queryPrompt.width + chromeStyle.space.s4 : root.textInset
                width: parent.width - x - root.textInset
                anchors.verticalCenter: parent.verticalCenter
                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("Search hosts, or type an address")
                font: chromeStyle.type.paletteInput
                color: chromeStyle.colors.paletteText
                background: Item {}
                padding: 0
                // Addresses and ports are typed here: nothing may turn a digit into a shortcut.
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhUrlCharactersOnly

                Label {
                    objectName: "connectHint"
                    x: filterField.cursorRectangle.x + filterField.cursorRectangle.width + chromeStyle.borderWidth
                    width: filterField.width - x - filterField.rightPadding
                    anchors.verticalCenter: parent.verticalCenter
                    visible: filterField.length === 0 && filterField.preeditText === ""
                    text: qsTr("Search hosts, or type ssh://… or tls://…")
                    font: filterField.font
                    color: chromeStyle.colors.paletteSecondaryText
                    elide: Text.ElideRight
                    Accessible.ignored: true
                }

                onTextChanged: if (root.picker) root.picker.filter = text

                Keys.onDownPressed: root.picker.step(1)
                Keys.onUpPressed: root.picker.step(-1)
                Keys.onReturnPressed: root.picker.activateCurrent()
                Keys.onEnterPressed: root.picker.activateCurrent()
                Keys.onEscapePressed: root.picker.cancel()
                Keys.onPressed: (event) => {
                    if (event.modifiers & Qt.ControlModifier) {
                        if (event.key === Qt.Key_J) {
                            root.picker.step(1);
                            event.accepted = true;
                        } else if (event.key === Qt.Key_K) {
                            root.picker.step(-1);
                            event.accepted = true;
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: chromeStyle.borderWidth
            color: chromeStyle.colors.paletteSeparator
        }
        // }}}

        // {{{ The rows
        ListView {
            id: list
            objectName: "connectList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.picker
            currentIndex: root.picker ? root.picker.currentIndex : -1
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: row

                required property int index
                required property string title
                required property string detail
                required property string tag
                required property string groupTitle
                required property string quick
                required property string target
                required property string rowFailure

                readonly property bool isCurrent: row.index === list.currentIndex

                width: ListView.view.width
                height: header.height + entry.height + failureBlock.height

                // The group's name, drawn by its first row.
                Item {
                    id: header
                    objectName: "connectGroupHeader"
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
                    objectName: "connectRow"
                    anchors.top: header.bottom
                    width: parent.width
                    height: chromeStyle.size.connectRow

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

                    Column {
                        x: root.textInset
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, accessory.x - chromeStyle.space.s4 - x)

                        Label {
                            objectName: "connectTitle"
                            width: parent.width
                            text: row.title
                            elide: Text.ElideRight
                            font: chromeStyle.type.body
                            color: entry.textColor
                        }
                        Label {
                            objectName: "connectDetail"
                            width: parent.width
                            text: row.detail
                            elide: Text.ElideRight
                            font: chromeStyle.type.footnote
                            color: entry.secondaryColor
                        }
                    }

                    // The transport tag and the quick-select key, which never shrink.
                    Row {
                        id: accessory
                        x: parent.width - width - root.textInset
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: chromeStyle.space.s4

                        Rectangle {
                            objectName: "connectTag"
                            visible: row.tag !== ""
                            anchors.verticalCenter: parent.verticalCenter
                            width: tagLabel.implicitWidth + chromeStyle.space.s4 + chromeStyle.space.s4
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

                        Label {
                            objectName: "connectQuick"
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.quick
                            font: chromeStyle.type.shortcut
                            color: entry.secondaryColor
                            Accessible.ignored: true
                        }
                    }

                    // Moving the pointer over a row selects it; a tap connects.
                    HoverHandler {
                        onHoveredChanged: if (hovered && root.picker) root.picker.currentIndex = row.index
                    }
                    TapHandler {
                        onTapped: {
                            root.picker.currentIndex = row.index;
                            root.picker.activate(row.index);
                        }
                    }
                }

                // What went wrong with this row's connection, in the words Attach to Host uses, with the way
                // out. The picker stays open.
                Item {
                    id: failureBlock
                    objectName: "connectFailure"
                    anchors.top: entry.bottom
                    width: parent.width
                    height: visible ? failureColumn.implicitHeight + chromeStyle.space.s4 : 0
                    visible: row.rowFailure !== ""

                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: row.rowFailure

                    Column {
                        id: failureColumn
                        x: root.textInset
                        width: parent.width - x - root.textInset
                        spacing: chromeStyle.space.s2

                        Label {
                            objectName: "connectFailureText"
                            width: parent.width
                            text: row.rowFailure
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            font: chromeStyle.type.footnote
                            color: chromeStyle.statusInk(chromeStyle.statusDanger)
                        }

                        Row {
                            spacing: chromeStyle.space.s4

                            Button {
                                objectName: "connectRetry"
                                text: qsTr("Retry")
                                flat: true
                                font: chromeStyle.type.footnote
                                onClicked: root.picker.retry()
                            }
                            Button {
                                objectName: "connectEditHost"
                                visible: root.picker !== null && root.picker.failureCanEditHost
                                text: qsTr("Edit Host…")
                                flat: true
                                font: chromeStyle.type.footnote
                                onClicked: root.picker.editFailedHost()
                            }
                        }
                    }
                }
            }
        }

        Label {
            objectName: "connectEmpty"
            visible: list.count === 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? chromeStyle.size.paletteRow : 0
            leftPadding: root.textInset
            verticalAlignment: Text.AlignVCenter
            text: qsTr("No matching targets")
            font: chromeStyle.type.body
            color: chromeStyle.colors.paletteSecondaryText
        }
        // }}}

        // {{{ The footer
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: chromeStyle.borderWidth
            color: chromeStyle.colors.paletteSeparator
        }

        RowLayout {
            objectName: "connectFooter"
            Layout.fillWidth: true
            Layout.leftMargin: root.textInset
            Layout.rightMargin: root.textInset
            Layout.topMargin: chromeStyle.space.s4
            Layout.bottomMargin: chromeStyle.space.s4
            spacing: chromeStyle.space.s4

            Button {
                objectName: "connectManageHosts"
                text: qsTr("Manage Hosts…")
                flat: true
                font: chromeStyle.type.footnote
                palette.buttonText: sys.buttonText
                onClicked: root.picker.manageHosts()
            }

            KitSwitch {
                objectName: "connectAskSwitch"
                checked: root.picker ? root.picker.askEveryTime : false
                name: qsTr("Ask every time Contour starts")
                onToggled: (checked) => root.picker.askEveryTime = checked
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Ask every time Contour starts")
                elide: Text.ElideRight
                font: chromeStyle.type.footnote
                color: chromeStyle.colors.paletteSecondaryText
                Accessible.ignored: true
            }

            Button {
                objectName: "connectCancel"
                text: qsTr("Cancel")
                font: chromeStyle.type.footnote
                palette.buttonText: sys.buttonText
                onClicked: root.picker.cancel()
            }

            Button {
                objectName: "connectConnect"
                text: root.picker && root.picker.isBusy ? qsTr("Connecting…") : qsTr("Connect")
                enabled: root.picker !== null && !root.picker.isBusy && list.count > 0
                font: chromeStyle.type.footnote
                highlighted: true
                palette.buttonText: sys.buttonText
                onClicked: root.picker.activateCurrent()
            }
        }
        // }}}
    }
}
