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
// Every size, colour and glyph comes from chromeStyle, so the same file draws the native rows in pixels
// and the Terminal row in cells.
import QtQuick
import QtQuick.Controls

Popup {
    id: root

    // The WindowController. Null-guarded EVERYWHERE below: the controller is destroyed before the QML
    // tree on window close, and QML re-evaluates dependent bindings once more against null during that
    // teardown. An unguarded `controller.` would raise a TypeError — which the run-wide QML message
    // gate in test_main.cpp turns into a failure of the whole test suite. (Same reason TabStrip.qml
    // guards its bindings.)
    required property var controller
    // The ApplicationWindow, so closing the palette can hand keyboard focus back to the terminal.
    required property var window

    // The palette's model, or null while the controller is gone.
    readonly property var model: root.controller ? root.controller.commandPalette : null
    // Where a row's text starts: the selection's inset from the list, then the same again inside it.
    readonly property real textInset: chromeStyle.space.paletteInset + chromeStyle.space.paletteInset
    // Whether the palette is inside a parameterised action's level.
    readonly property bool inLevel: root.model !== null && root.model.depth !== root.model.topDepth

    // Where the palette goes (window::placePalette): centred, 18 % down, never past the window's margins.
    readonly property var placement: chromeStyle.palettePlacement(root.window ? root.window.width : 0,
                                                                  root.window ? root.window.height : 0)

    modal: true
    focus: true
    padding: 0
    // Not CloseOnEscape: Escape inside a level goes back one level (the field's handler), and the
    // Popup's own Escape shortcut would take the key first and close the palette instead.
    closePolicy: Popup.CloseOnPressOutside

    parent: Overlay.overlay
    x: root.placement.x
    y: root.placement.y
    width: root.placement.width
    // Fits its content, up to the placement's height.
    height: Math.min(root.placement.maxHeight,
                     queryRow.height + topRule.height + list.contentHeight + empty.height
                     + bottomRule.height + footer.height)

    background: PopupSurface {
        color: chromeStyle.colors.paletteSurface
        radius: chromeStyle.radii.popover
        border.color: chromeStyle.colors.paletteSeparator
        elevation: chromeStyle.elevation.e3
    }

    // Opens with a fade and a slight scale, ease-out; closes with a quicker fade, ease-in. Under Reduce Motion
    // the tokens make the scale start at 1 and both durations the cross-fade's, so no branch is needed here.
    enter: Transition {
        NumberAnimation {
            objectName: "commandPaletteEnterFade"
            property: "opacity"
            from: 0
            to: 1
            duration: chromeStyle.motion.standard
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            objectName: "commandPaletteEnterScale"
            property: "scale"
            from: chromeStyle.motion.popoverScale
            to: 1
            duration: chromeStyle.motion.standard
            easing.type: Easing.OutCubic
        }
    }
    exit: Transition {
        NumberAnimation {
            id: exitFade
            objectName: "commandPaletteExitFade"
            property: "opacity"
            to: 0
            duration: chromeStyle.motion.fast
            easing.type: Easing.InCubic
        }
    }

    // The exit transition's fade, for the tests: unlike the enter transition's, its animation is not a
    // QObject child of the popup, so findChild() cannot reach it.
    readonly property var _exitFade: exitFade

    // What had the keyboard when the palette opened. A dialog or popup (the find bar, a flyout, About)
    // gets it back on close; anything else is the terminal, which restoreTerminalFocus() reaches.
    property Item _returnFocus: null

    onAboutToShow: root._returnFocus = root.window ? root.window.activeFocusItem : null

    // Whether @p item sits in the window's overlay layer, i.e. inside a popup or a dialog.
    function _isOverOverlay(item) {
        const overlay = root.Overlay.overlay;
        for (let node = item; node; node = node.parent)
            if (node === overlay)
                return true;
        return false;
    }

    // Refresh + focus on every open, not just the first: the model's rows are rebuilt by the
    // controller (open tabs change), and the filter must start empty with the caret already in it so
    // the user can type immediately.
    onOpened: {
        filterField.clear();
        filterField.forceActiveFocus();
        root.select(list.count > 0 ? 0 : -1);
    }

    // The command the user picked, held until this popup has actually closed — see onClosed.
    property string pendingCommandId: ""

    // Whatever closed the palette — Escape, a pick, a click outside — the keyboard must go back where it
    // was: to the dialog or popup the palette opened over, else to the terminal, or the user is left typing
    // into nothing. Only THEN does the picked command run: a command
    // that opens a keyboard-driven surface of its own (SetTabColor's swatch picker, SetTabTitle's rename
    // field) takes the focus as it opens, and running it any earlier means this restore takes that focus
    // straight back and the surface cannot be typed into at all. Same law, and the same shape, as
    // ActionContextMenu.qml's _pendingActionId: act once the popup is genuinely gone.
    //
    // Qt.callLater, not a direct call: "genuinely gone" is not yet true INSIDE this handler. Qt emits
    // closed() from within the popup's own exit transition, so a command that re-opens the palette —
    // OpenCommandPalette, a row this palette itself always offers — would re-enter Popup.open() on top of
    // the close that is still unwinding, and the palette would need dismissing twice. Deferring to the
    // next event-loop turn dispatches the command with the popup's state machine at rest.
    onClosed: {
        if (root._returnFocus && root._isOverOverlay(root._returnFocus))
            root._returnFocus.forceActiveFocus();
        else if (root.window)
            root.window.restoreTerminalFocus();
        root._returnFocus = null;
        const commandId = root.pendingCommandId;
        root.pendingCommandId = ""; // cleared BEFORE dispatch: the command may re-open this palette
        if (commandId && root.controller)
            Qt.callLater(root.dispatch, commandId);
    }

    // The deferred half of onClosed. Re-checks the controller: the window can be torn down between the
    // close and the event-loop turn that gets here.
    function dispatch(commandId) {
        if (root.controller)
            root.controller.runCommand(commandId);
    }

    // Takes the currently highlighted command and closes; onClosed above is what runs it. Reads the id
    // off the highlighted DELEGATE rather than indexing the model by role number, so there is no magic
    // 257 here to drift out of step with CommandPaletteModel::Roles. A null currentItem (an empty
    // filtered list, or a row not yet realized) does nothing rather than running an arbitrary command.
    function acceptCurrent() {
        if (!list.currentItem)
            return;
        root.pendingCommandId = list.currentItem.commandId;
        root.close();
    }

    // What Enter and a click do: a drill-in row opens its level, any other row runs.
    function activateCurrent() {
        if (!list.currentItem || !root.model)
            return;
        if (list.currentItem.kind === root.model.drillInKind)
            root.model.enter(list.currentIndex);
        else
            root.acceptCurrent();
    }

    // Escape: back one level inside one, else close.
    function stepBack() {
        if (root.inLevel)
            root.model.back();
        else
            root.close();
    }

    // Selects @p index and keeps it in view — the arrow keys walk a 200-row level past the viewport.
    function select(index) {
        list.currentIndex = index;
        if (index >= 0)
            list.positionViewAtIndex(index, ListView.Contain);
    }

    // The group headers' font: the footnote type at Font.DemiBold, a copy, since the token's font is shared
    // and a font cannot be both assigned and tweaked (as KitDialog's heading).
    readonly property font headerFont: {
        let header = chromeStyle.type.footnote;
        header.weight = Font.DemiBold;
        return header;
    }

    // Renders a command title as StyledText with the filter's matched characters in bold, so the user
    // sees WHY a row matched. In the row's own colour on every row, selected or not: an accent tint does
    // not read on the light surface with the default accent.
    //
    // The whole string is HTML-escaped (a live tab title can contain & < >), so it is always valid
    // StyledText. `matches` are UTF-16 code-unit indices (the model converts them from the fuzzy filter's
    // UTF-8 byte offsets), so they line up with charAt() even when a title contains multibyte characters.
    function highlightedTitle(text, matches) {
        var next = 0;
        var result = "";
        for (var i = 0; i < text.length; ++i) {
            var hit = matches && next < matches.length && matches[next] === i;
            if (hit) {
                result += "<b>";
                ++next;
            }
            var c = text.charAt(i);
            if (c === "&")
                result += "&amp;";
            else if (c === "<")
                result += "&lt;";
            else if (c === ">")
                result += "&gt;";
            else
                result += c;
            if (hit)
                result += "</b>";
        }
        return result;
    }

    // A level change resets the model: the field takes the model's filter FIRST (its onTextChanged selects
    // row 0), and only then the anchor — the parent's row after back() — is selected.
    Connections {
        target: root.model
        function onLevelChanged() {
            filterField.text = root.model.filter;
            root.select(root.model.anchorRow < list.count ? root.model.anchorRow : -1);
        }
    }

    contentItem: Column {
        spacing: 0

        // {{{ The query row: the prompt glyph, the level chip, the field
        Item {
            id: queryRow
            objectName: "commandPaletteQuery"
            width: parent.width
            height: chromeStyle.size.paletteField

            Accessible.role: Accessible.EditableText
            Accessible.name: qsTr("Command")

            Row {
                id: queryLead
                x: chromeStyle.space.paletteInset
                anchors.verticalCenter: parent.verticalCenter
                spacing: chromeStyle.space.s2

                Label {
                    objectName: "commandPalettePrompt"
                    visible: chromeStyle.palettePromptGlyph !== ""
                    text: chromeStyle.palettePromptGlyph
                    font: chromeStyle.type.paletteInput
                    color: chromeStyle.colors.paletteSecondaryText
                    Accessible.ignored: true
                }

                Label {
                    objectName: "commandPaletteLevel"
                    visible: root.inLevel
                    text: root.model ? root.model.levelTitle + " " + chromeStyle.drillInGlyph : ""
                    font: chromeStyle.type.paletteInput
                    color: chromeStyle.colors.paletteSecondaryText
                }
            }

            TextField {
                id: filterField
                objectName: "commandPaletteFilter"
                x: queryLead.x + (queryLead.width > 0 ? queryLead.width + queryLead.spacing : 0)
                width: parent.width - x - chromeStyle.space.paletteInset
                anchors.verticalCenter: parent.verticalCenter
                placeholderText: qsTr("Type a command or search…")
                font: chromeStyle.type.paletteInput
                color: chromeStyle.colors.paletteText
                placeholderTextColor: chromeStyle.colors.paletteSecondaryText
                // The palette's surface is the field's: no frame of its own.
                background: Item {}
                padding: 0

                // The model re-ranks (and regroups) on every keystroke.
                onTextChanged: {
                    if (root.model)
                        root.model.filter = text;
                    root.select(list.count > 0 ? 0 : -1);
                }

                // Arrow keys move the SELECTION while the caret stays in the field, so the user never
                // has to leave the box to pick a row.
                Keys.onDownPressed: root.select(Math.min(list.currentIndex + 1, list.count - 1))
                Keys.onUpPressed: root.select(Math.max(list.currentIndex - 1, 0))
                Keys.onReturnPressed: root.activateCurrent()
                Keys.onEnterPressed: root.activateCurrent()
                Keys.onEscapePressed: root.stepBack()

                // Vim-style navigation: Ctrl+J/Ctrl+K move the selection down/up, the same as the arrow
                // keys, accepted so Ctrl+J cannot fall through as a line feed into the filter text.
                // Backspace in an empty field leaves a level; with text it edits the text.
                Keys.onPressed: (event) => {
                    if (event.modifiers & Qt.ControlModifier) {
                        if (event.key === Qt.Key_J) {
                            root.select(Math.min(list.currentIndex + 1, list.count - 1));
                            event.accepted = true;
                        } else if (event.key === Qt.Key_K) {
                            root.select(Math.max(list.currentIndex - 1, 0));
                            event.accepted = true;
                        }
                    } else if (event.key === Qt.Key_Backspace && text === "" && root.inLevel) {
                        root.model.back();
                        event.accepted = true;
                    }
                }
            }
        }

        Rectangle {
            id: topRule
            width: parent.width
            height: chromeStyle.borderWidth
            color: chromeStyle.colors.paletteSeparator
        }
        // }}}

        // {{{ The rows
        ListView {
            id: list
            objectName: "commandPaletteList"
            width: parent.width
            // Takes whatever the query row, the rules and the footer leave.
            height: Math.max(0, root.height - queryRow.height - topRule.height - empty.height
                                - bottomRule.height - footer.height)
            clip: true
            // Null-guarded: the controller dies before this tree on window close (see `controller`).
            model: root.model
            currentIndex: 0
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar {}

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
                required property var titleMatches

                readonly property bool isCurrent: row.index === list.currentIndex
                readonly property bool drillsIn: root.model !== null && row.kind === root.model.drillInKind

                width: ListView.view.width
                height: header.height + entry.height

                // {{{ Group header — drawn by the FIRST row of each group.
                Item {
                    id: header
                    objectName: "commandPaletteGroupHeader"
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
                        font: root.headerFont
                        elide: Text.ElideRight
                        color: chromeStyle.colors.paletteSecondaryText
                        Accessible.ignored: true
                    }
                }
                // }}}

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

                    readonly property color textColor: row.isCurrent ? chromeStyle.colors.paletteSelectionText
                                                                     : chromeStyle.colors.paletteText
                    readonly property color secondaryColor: row.isCurrent
                                                            ? chromeStyle.colors.paletteSelectionText
                                                            : chromeStyle.colors.paletteSecondaryText

                    Rectangle {
                        objectName: "commandPaletteSelection"
                        anchors.fill: parent
                        anchors.leftMargin: chromeStyle.space.paletteInset
                        anchors.rightMargin: chromeStyle.space.paletteInset
                        radius: chromeStyle.radii.control
                        visible: row.isCurrent
                        color: chromeStyle.colors.paletteSelectionFill
                    }

                    Label {
                        id: titleLabel
                        objectName: "commandPaletteTitle"
                        x: root.textInset
                        anchors.verticalCenter: parent.verticalCenter
                        // Yields to the accessory, which never shrinks: the title is the part that can be
                        // arbitrarily long (a shell-set tab title).
                        width: Math.max(0, accessory.x - chromeStyle.space.s2 - x)
                        textFormat: Text.StyledText
                        text: root.highlightedTitle(row.title, row.titleMatches)
                        // A right-to-left title starts at the row's leading edge like every other.
                        horizontalAlignment: Text.AlignLeft
                        elide: Text.ElideRight
                        font: chromeStyle.type.body
                        color: entry.textColor
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
                            color: entry.secondaryColor
                        }

                        Label {
                            id: drillIn
                            objectName: "commandPaletteDrillIn"
                            visible: row.drillsIn
                            anchors.verticalCenter: parent.verticalCenter
                            text: chromeStyle.drillInGlyph
                            font: chromeStyle.type.body
                            color: entry.secondaryColor
                            Accessible.ignored: true
                        }
                    }

                    HoverHandler {
                        // Hovering moves the selection, so the mouse and the keyboard agree on what
                        // "current" means and a click can never act on a row other than the highlighted one.
                        onHoveredChanged: if (hovered) list.currentIndex = row.index
                    }

                    TapHandler {
                        onTapped: {
                            list.currentIndex = row.index;
                            root.activateCurrent();
                        }
                    }
                }
                // }}}
            }
        }

        Label {
            id: empty
            objectName: "commandPaletteEmpty"
            visible: list.count === 0
            width: parent.width
            height: visible ? chromeStyle.size.paletteRow : 0
            leftPadding: root.textInset
            verticalAlignment: Text.AlignVCenter
            text: qsTr("No matching commands")
            font: chromeStyle.type.body
            color: chromeStyle.colors.paletteSecondaryText
        }
        // }}}

        // {{{ The footer: the selected row's description, and what Enter and Escape do.
        Rectangle {
            id: bottomRule
            width: parent.width
            height: chromeStyle.borderWidth
            color: chromeStyle.colors.paletteSeparator
        }

        Item {
            id: footer
            objectName: "commandPaletteFooter"
            width: parent.width
            height: chromeStyle.size.paletteFooter

            Label {
                id: footerHints
                objectName: "commandPaletteFooterHints"
                x: parent.width - width - root.textInset
                anchors.verticalCenter: parent.verticalCenter
                // Reads depth and count so the binding re-runs on a level change and a re-rank; the
                // invokable itself is opaque to the binding engine.
                text: root.model && root.model.depth >= 0 && list.count >= 0
                      ? root.model.footerHints(list.currentIndex) : ""
                font: chromeStyle.type.footnote
                color: chromeStyle.colors.paletteSecondaryText
            }

            Label {
                objectName: "commandPaletteFooterDescription"
                x: root.textInset
                width: Math.max(0, footerHints.x - chromeStyle.space.s4 - x)
                anchors.verticalCenter: parent.verticalCenter
                text: list.currentItem ? list.currentItem.description : ""
                horizontalAlignment: Text.AlignLeft
                elide: Text.ElideRight
                font: chromeStyle.type.footnote
                color: chromeStyle.colors.paletteSecondaryText
            }
        }
        // }}}
    }
}
