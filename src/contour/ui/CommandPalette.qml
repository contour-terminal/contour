// vim:syntax=qml
// The command palette: a searchable popup listing every runnable command.
//
// Opened by the OpenCommandPalette action (Ctrl+Shift+P by default), which routes through the session
// manager to this window's WindowController, whose `commandPaletteRequested` signal main.qml connects
// to open(). The rows come from the controller's CommandPaletteModel:
//
//   - with an empty filter the list is sectioned — RECENTLY USED (persisted across restarts) on top,
//     then ALL COMMANDS alphabetically;
//   - as soon as the user types, the model collapses both into one fuzzy-ranked list and clears
//     `sectioned`, so the headers disappear on their own without this file deciding when.
//
// Each row shows the command's title, its key binding right-aligned (so the user learns the shortcut
// for next time), and its documentation beneath.
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

    modal: true
    focus: true
    padding: 0

    // Centered, and sized to the window rather than to the content: a list whose width shifted with
    // the longest visible command would jump on every keystroke.
    anchors.centerIn: Overlay.overlay
    width: Math.min(720, root.window ? root.window.width * 0.9 : 720)
    height: Math.min(520, root.window ? root.window.height * 0.8 : 520)

    // Live OS palette handle so the popup follows dark/light in realtime (see TabColorFlyout).
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    background: Rectangle {
        color: systemPalette.window
        border.color: systemPalette.mid
        border.width: 1
        radius: 6
    }

    // Refresh + focus on every open, not just the first: the model's rows are rebuilt by the
    // controller (open tabs change), and the filter must start empty with the caret already in it so
    // the user can type immediately.
    onOpened: {
        filterField.clear();
        filterField.forceActiveFocus();
        list.currentIndex = list.count > 0 ? 0 : -1;
    }

    // The command the user picked, held until this popup has actually closed — see onClosed.
    property string pendingCommandId: ""

    // Whatever closed the palette — Escape, a pick, a click outside — the terminal must get the keyboard
    // back, or the user is left typing into nothing. Only THEN does the picked command run: a command
    // that opens a keyboard-driven surface of its own (SetTabColor's swatch picker, SetTabTitle's rename
    // field) takes the focus as it opens, and running it any earlier means this restore takes that focus
    // straight back and the surface cannot be typed into at all. Same law, and the same shape, as
    // TabContextMenu.qml's colorPending: act once the popup is genuinely gone.
    //
    // Qt.callLater, not a direct call: "genuinely gone" is not yet true INSIDE this handler. Qt emits
    // closed() from within the popup's own exit transition, so a command that re-opens the palette —
    // OpenCommandPalette, a row this palette itself always offers — would re-enter Popup.open() on top of
    // the close that is still unwinding, and the palette would need dismissing twice. Deferring to the
    // next event-loop turn dispatches the command with the popup's state machine at rest.
    onClosed: {
        if (root.window)
            root.window.restoreTerminalFocus();
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

    // Renders a command title as StyledText with the filter's matched characters emphasized, so the user
    // sees WHY a row matched. `matches` is the ascending list of title indices the model's fuzzy filter
    // landed on (empty when unfiltered). Matched characters are always bold; on an unselected row they
    // also take the accent colour, but on the SELECTED row — whose background is already that accent —
    // bold alone is used, since a tint there would wash out. The colour is chosen here, in the view,
    // because only QML knows the live OS accent and which row is current.
    //
    // The whole string is HTML-escaped (a live tab title can contain & < >), so it is always valid
    // StyledText. `matches` are UTF-16 code-unit indices (the model converts them from the fuzzy filter's
    // UTF-8 byte offsets), so they line up with charAt() even when a title contains multibyte characters.
    function highlightedTitle(text, matches, selected) {
        var open = selected ? "<b>" : "<b><font color=\"" + systemPalette.highlight.toString() + "\">";
        var close = selected ? "</b>" : "</font></b>";
        var next = 0;
        var result = "";
        for (var i = 0; i < text.length; ++i) {
            var hit = matches && next < matches.length && matches[next] === i;
            if (hit) {
                result += open;
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
                result += close;
        }
        return result;
    }

    contentItem: Column {
        spacing: 0

        // {{{ Filter box
        Item {
            id: filterRow
            width: parent.width
            height: filterField.height + 12

            TextField {
                id: filterField
                objectName: "commandPaletteFilter"
                x: 8
                y: 6
                width: parent.width - 16
                placeholderText: qsTr("Type to search commands…")
                // The model re-ranks on every keystroke; `sectioned` follows from the same write, so
                // the section headers vanish as soon as there is a query.
                onTextChanged: {
                    if (root.controller && root.controller.commandPalette)
                        root.controller.commandPalette.filter = text;
                    list.currentIndex = list.count > 0 ? 0 : -1;
                }

                // Arrow keys move the SELECTION while the caret stays in the field, so the user never
                // has to leave the box to pick a row. Enter runs, Escape dismisses.
                Keys.onDownPressed: list.incrementCurrentIndex()
                Keys.onUpPressed: list.decrementCurrentIndex()
                Keys.onReturnPressed: root.acceptCurrent()
                Keys.onEnterPressed: root.acceptCurrent()
                Keys.onEscapePressed: root.close()

                // Vim-style navigation: Ctrl+J/Ctrl+K move the selection down/up, the same as the arrow
                // keys, so a home-row user never reaches for them. Accepted explicitly so Ctrl+J cannot
                // fall through as a line feed into the filter text. Other keys are left unaccepted here
                // and reach the named handlers above and normal text entry.
                Keys.onPressed: (event) => {
                    if (event.modifiers & Qt.ControlModifier) {
                        if (event.key === Qt.Key_J) {
                            list.incrementCurrentIndex();
                            event.accepted = true;
                        } else if (event.key === Qt.Key_K) {
                            list.decrementCurrentIndex();
                            event.accepted = true;
                        }
                    }
                }
            }
        }

        Rectangle {
            id: separator
            width: parent.width
            height: 1
            color: systemPalette.mid
        }
        // }}}

        // {{{ Command list
        ListView {
            id: list
            objectName: "commandPaletteList"
            width: parent.width
            // Takes whatever the filter row and its separator leave, so the two cannot disagree about
            // the total and leave the last command clipped under the popup's bottom edge.
            height: root.height - filterRow.height - separator.height
            clip: true
            // Null-guarded: the controller dies before this tree on window close (see `controller`).
            model: root.controller ? root.controller.commandPalette : null
            currentIndex: 0
            // Keeps the highlighted row on screen as the arrow keys walk past the viewport edge.
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
                required property int section
                required property bool sectionStart
                required property var titleMatches

                width: ListView.view.width
                height: header.height + entry.height

                // {{{ Section header — drawn by the FIRST row of each section.
                //
                // The model marks that row (`sectionStart`) rather than this delegate looking backwards
                // at its predecessor, which a virtualized ListView cannot reliably do. Collapses to zero
                // height when the list is filtered (the model reports one flat section then), so the
                // headers disappear without this file re-deriving "is there a query?".
                Item {
                    id: header
                    width: parent.width
                    height: visible ? 24 : 0
                    visible: row.sectionStart

                    Label {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.section === 0 ? qsTr("RECENTLY USED") : qsTr("ALL COMMANDS")
                        font.pixelSize: 10
                        font.bold: true
                        color: systemPalette.mid
                    }
                }
                // }}}

                // {{{ The command itself: title + right-aligned shortcut, description beneath.
                Rectangle {
                    id: entry
                    anchors.top: header.bottom
                    width: parent.width
                    height: 46
                    color: row.index === list.currentIndex ? systemPalette.highlight : "transparent"

                    property color textColor: row.index === list.currentIndex
                                              ? systemPalette.highlightedText
                                              : systemPalette.windowText

                    Label {
                        id: titleLabel
                        objectName: "commandPaletteTitle"
                        x: 12
                        y: 5
                        // Yields to the shortcut rather than overrunning it: the shortcut is short and
                        // fixed, the title is the part that can be arbitrarily long.
                        width: parent.width - 24 - shortcutLabel.width - 8
                        // StyledText so the filter's matched characters can be bolded/accented; the raw
                        // title and its matched indices come from the model, the styling is chosen here.
                        // Re-evaluates when the selection moves, flipping the accent tint off the current
                        // row (see highlightedTitle).
                        textFormat: Text.StyledText
                        text: root.highlightedTitle(row.title, row.titleMatches,
                                                    row.index === list.currentIndex)
                        elide: Text.ElideRight
                        color: entry.textColor
                    }

                    Label {
                        id: shortcutLabel
                        objectName: "commandPaletteShortcut"
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        y: 5
                        text: row.shortcut
                        font.family: "monospace"
                        font.pixelSize: 11
                        // Dimmed against the title: it is a hint for next time, not the thing being
                        // chosen now.
                        opacity: 0.75
                        color: entry.textColor
                    }

                    Label {
                        objectName: "commandPaletteDescription"
                        x: 12
                        anchors.top: titleLabel.bottom
                        anchors.topMargin: 1
                        width: parent.width - 24
                        text: row.description
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        opacity: 0.7
                        color: entry.textColor
                    }

                    HoverHandler {
                        // Hovering moves the selection, so the mouse and the keyboard agree on what
                        // "current" means and a click can never run a row other than the highlighted one.
                        onHoveredChanged: if (hovered) list.currentIndex = row.index
                    }

                    TapHandler {
                        onTapped: {
                            list.currentIndex = row.index;
                            root.acceptCurrent();
                        }
                    }
                }
                // }}}
            }
        }
        // }}}
    }
}
