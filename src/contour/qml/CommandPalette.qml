// vim:syntax=qml
// The command palette: a searchable popup listing every runnable command.
//
// Opened by the OpenCommandPalette action (Ctrl+Shift+P by default), which routes through the session
// manager to this window's WindowController, whose `commandPaletteRequested` signal Main.qml connects
// to open(). The rows come from the controller's CommandPaletteModel:
//
//   - with an empty filter the list is grouped — Recently used (persisted across restarts) on top,
//     then the command groups (Pane, Tab, …), each alphabetically;
//   - as soon as the user types, the model ranks across the groups and orders them by their best match.
//   The model names each group on its first row (`groupTitle`), so this file never decides where a
//   header goes.
//
// A parameterised action (Change Profile, Launch Layout, …) is one row marked with the drill-in glyph;
// Enter on it lists its children (the model's level), Escape or Backspace in an empty field goes back.
// The selected row's description shows in the footer, beside the key hints for what Enter and Escape do.
//
// The frame -- surface, query row, list, footer, motion, the keyboard's hand-back -- is PickerPopup's, shared
// with the switcher; this file is the palette's model wiring, its keys and its row. Every size, colour and
// glyph comes from chromeStyle, so the same file draws the native rows in pixels and the Terminal row in cells.
import QtQuick
import QtQuick.Controls

PickerPopup {
    id: root

    // The WindowController. Null-guarded EVERYWHERE below: the controller is destroyed before the QML
    // tree on window close, and QML re-evaluates dependent bindings once more against null during that
    // teardown. An unguarded `controller.` would raise a TypeError — which the run-wide QML message
    // gate in test_main.cpp turns into a failure of the whole test suite. (Same reason TabStrip.qml
    // guards its bindings.)
    required property var controller

    // The palette's model, or null while the controller is gone.
    readonly property var paletteModel: root.controller ? root.controller.commandPalette : null
    // Whether the palette is inside a parameterised action's level.
    readonly property bool inLevel: root.paletteModel !== null
                                    && root.paletteModel.depth !== root.paletteModel.topDepth

    namePrefix: "commandPalette"
    model: root.paletteModel
    groupCount: root.paletteModel ? root.paletteModel.groupCount : 0
    levelTitle: root.inLevel ? root.paletteModel.levelTitle : ""
    hintText: qsTr("Type a command or search…")
    emptyText: qsTr("No matching commands")
    footerDescription: root.list.currentItem ? root.list.currentItem.description : ""
    footerHints: root.list.currentItem ? root.list.currentItem.footerHints : ""
    fieldName: qsTr("Command")

    // The model holds off its refresh while the palette is on screen; it refreshes as the palette starts to
    // show, before the field is cleared.
    onOpening: {
        if (root.paletteModel)
            root.paletteModel.shown = true;
    }

    // A palette destroyed while open (its window closing) must not leave refresh() held off for good.
    Component.onDestruction: {
        if (root.paletteModel)
            root.paletteModel.shown = false;
    }

    // Off screen from the moment it starts to close, so an open asked for during the close refreshes.
    onAboutToHide: {
        if (root.paletteModel)
            root.paletteModel.shown = false;
    }

    // The model re-ranks (and regroups) on every keystroke.
    onQueryEdited: (text) => {
        if (root.paletteModel)
            root.paletteModel.filter = text;
    }

    // Moving the pointer moves the selection, so the mouse and the keyboard agree on what "current" means and
    // a click can never act on a row other than the highlighted one.
    onPointerMoved: (index) => {
        if (index >= 0)
            root.list.currentIndex = index;
    }

    // The picked command runs once the palette has closed and the keyboard is back (PickerPopup.closeThen).
    // Re-checks the controller: the window can be torn down between the close and the event-loop turn that
    // gets here.
    onPicked: (commandId) => {
        if (commandId && root.controller)
            root.controller.runCommand(commandId);
    }

    // Takes the currently highlighted command and closes. Reads the id off the highlighted DELEGATE rather
    // than indexing the model by role number, so there is no magic 257 here to drift out of step with
    // CommandPaletteModel::Roles. A null currentItem (an empty filtered list, or a row not yet realized) does
    // nothing rather than running an arbitrary command.
    function acceptCurrent() {
        if (!root.list.currentItem)
            return;
        root.closeThen(root.list.currentItem.commandId);
    }

    // What Enter and a click do: a drill-in row opens its level, any other row runs.
    function activateCurrent() {
        if (!root.list.currentItem || !root.paletteModel)
            return;
        if (root.list.currentItem.kind === root.paletteModel.drillInKind)
            root.paletteModel.enter(root.list.currentIndex);
        else
            root.acceptCurrent();
    }

    // Escape: back one level inside one, else close.
    function stepBack() {
        if (root.inLevel)
            root.paletteModel.back();
        else
            root.close();
    }

    // The arrows and Ctrl+J/Ctrl+K (vim-style, accepted so Ctrl+J cannot fall through as a line feed into the
    // filter text) move the selection; Enter activates; Escape steps back; a bare Backspace in an empty field
    // leaves a level (with a modifier, or with text, it is the field's).
    onKeyPressed: (event) => {
        const control = (event.modifiers & Qt.ControlModifier) !== 0;
        if (event.key === Qt.Key_Down || (control && event.key === Qt.Key_J))
            root.step(1);
        else if (event.key === Qt.Key_Up || (control && event.key === Qt.Key_K))
            root.step(-1);
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
            root.activateCurrent();
        else if (event.key === Qt.Key_Escape)
            root.stepBack();
        else if (event.key === Qt.Key_Backspace && event.modifiers === Qt.NoModifier && root.field.text === ""
                 && root.inLevel)
            root.paletteModel.back();
        else
            return;
        event.accepted = true;
    }

    // A level change resets the model: the field takes the model's filter FIRST (its onTextChanged selects
    // row 0), and only then the anchor — the parent's row after back() — is selected.
    Connections {
        target: root.paletteModel
        function onLevelChanged() {
            root.field.text = root.paletteModel.filter;
            root.select(root.paletteModel.anchorRow < root.list.count ? root.paletteModel.anchorRow : -1);
        }
        // The field shows the model's query whoever set it (a refresh, a test, the controller); setting it
        // to what it already holds changes nothing.
        function onFilterChanged() {
            if (root.field.text !== root.paletteModel.filter)
                root.field.text = root.paletteModel.filter;
        }
    }

    delegate: Item {
        id: row

        // Declared `required` so a missing role is a loud error rather than an `undefined`
        // silently rendering as an empty label.
        required property int index
        required property string commandId
        required property string title
        required property string description
        required property string shortcut
        required property string groupTitle
        required property int kind
        required property string footerHints
        required property var titleMatches

        readonly property bool isCurrent: row.index === root.list.currentIndex
        readonly property bool drillsIn: root.paletteModel !== null && row.kind === root.paletteModel.drillInKind

        width: ListView.view.width
        height: header.height + entry.height
        // Where the row itself starts below its group header, for the list's hover.
        readonly property real headerHeight: header.height

        // Group header — drawn by the FIRST row of each group.
        PickerGroupHeader {
            id: header
            width: parent.width
            namePrefix: root.namePrefix
            title: row.groupTitle
            textInset: root.textInset
        }

        // {{{ The row: title, then its shortcut or the drill-in glyph.
        Item {
            id: entry
            objectName: "commandPaletteRow"
            anchors.top: header.bottom
            width: parent.width
            height: chromeStyle.size.paletteRow

            Accessible.role: Accessible.ListItem
            Accessible.name: row.title
            Accessible.description: row.drillsIn ? qsTr("Opens a list") : ""

            PickerRowSelection {
                id: selection
                anchors.fill: parent
                anchors.leftMargin: chromeStyle.space.paletteInset
                anchors.rightMargin: chromeStyle.space.paletteInset
                namePrefix: root.namePrefix
                selected: row.isCurrent
            }

            MatchedLabel {
                id: titleLabel
                objectName: "commandPaletteTitle"
                x: root.textInset
                anchors.verticalCenter: parent.verticalCenter
                // Yields to the accessory, which never shrinks: the title is the part that can be
                // arbitrarily long (a shell-set tab title).
                width: Math.max(0, accessory.x - chromeStyle.space.s2 - x)
                plainText: row.title
                matches: row.titleMatches
                // A right-to-left title starts at the row's leading edge like every other.
                horizontalAlignment: Text.AlignLeft
                elide: Text.ElideRight
                font: chromeStyle.type.body
                color: selection.ink
            }

            Item {
                id: accessory
                objectName: "commandPaletteAccessory"
                x: parent.width - width - root.textInset
                width: row.drillsIn ? drillIn.implicitWidth : shortcutLabel.implicitWidth
                height: parent.height

                Label {
                    id: shortcutLabel
                    objectName: "commandPaletteShortcut"
                    visible: !row.drillsIn
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.shortcut
                    font: chromeStyle.type.shortcut
                    color: selection.secondaryInk
                }

                Label {
                    id: drillIn
                    objectName: "commandPaletteDrillIn"
                    visible: row.drillsIn
                    anchors.verticalCenter: parent.verticalCenter
                    text: chromeStyle.drillInGlyph
                    font: chromeStyle.type.body
                    color: selection.secondaryInk
                    Accessible.ignored: true
                }
            }

            TapHandler {
                onTapped: {
                    root.list.currentIndex = row.index;
                    root.activateCurrent();
                }
            }
        }
        // }}}
    }
}
