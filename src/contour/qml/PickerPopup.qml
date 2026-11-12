// vim:syntax=qml
// The command palette's frame, shared by every in-scene picker of its family (the palette, the switcher):
// the surface, the query row (prompt glyph, level chip, field and its drawn hint), the list, the empty state,
// the footer (the selected row's description and its key hints), the enter and exit motion, and handing the
// keyboard back on close.
//
// It decides nothing. The owner gives it the model and the row delegate, the texts it shows, and answers its
// signals: every key typed into the field is offered to the owner first (keyPressed), what the user typed is
// reported (queryEdited), and a pick is run only once the popup has closed (closeThen → picked).
//
// Every part is named `namePrefix` + its role ("Filter", "List", "Query", "Prompt", "Level", "Hint", "Empty",
// "Footer", "FooterHints", "FooterDescription", "EnterFade", "EnterScale", "ExitFade"), so a test finds the
// palette's parts as `commandPalette…` and the switcher's as `switcher…`.
//
// Every size, colour and glyph comes from chromeStyle, so the same file draws the native rows in pixels and
// the Terminal row in cells.
import QtQuick
import QtQuick.Controls

Popup {
    id: root

    /// The ApplicationWindow, so closing the picker can hand keyboard focus back to the terminal. Null-guarded:
    /// a window torn down re-evaluates the bindings once more against null.
    required property var window

    /// The prefix of every part's objectName.
    property string namePrefix: "picker"
    /// The control the picker hangs from, in the window's coordinates (a rect), or null to centre it the way
    /// the palette is placed (window::placePalette).
    property var anchorRect: null
    /// The level the picker is in, shown before the field as "‹title› ›"; empty at the top level.
    property string levelTitle: ""
    /// The field's drawn hint, shown while it is empty.
    property string hintText: ""
    /// The field's accessible name.
    property string fieldName: ""
    /// What the list says when it has no row.
    property string emptyText: ""
    /// The footer's leading text: what the selected row does.
    property string footerDescription: ""
    /// The footer's trailing text: the keys for the selected row.
    property string footerHints: ""
    /// The rows.
    property alias model: listView.model
    /// The row delegate. It reports `headerHeight` (the group header above its row, 0 for none), so the pointer
    /// over a header selects nothing.
    property alias delegate: listView.delegate
    /// The list, for the owner's delegate and selection.
    property alias list: listView
    /// The filter field.
    property alias field: filterField

    /// Where a row's text starts: the selection's inset from the list, then the same again inside it.
    readonly property real textInset: chromeStyle.space.paletteInset + chromeStyle.space.paletteInset

    /// Moved the selection by @p delta rows (an arrow key, Ctrl+J/Ctrl+K).
    signal stepped(int delta)
    /// A key typed into the field, offered to the owner before the field: accept it to take it.
    signal keyPressed(var event)
    /// The field's text changed, as the user typed or as anyone set it.
    signal queryEdited(string text)
    /// The pointer moved, really moved, over row @p index of the list (-1: over no selectable row). A list
    /// rebuilt under a resting pointer, or a picker opening under one, reports nothing.
    signal pointerMoved(int index)
    /// The picker starts to show: the owner refreshes its rows here, before the field is cleared.
    signal opening()
    /// The picker has closed and the keyboard is back where it was: run @p payload, given to closeThen().
    signal picked(var payload)

    // Where the picker goes: centred like the palette (window::placePalette), or hanging from anchorRect
    // (window::popupBelow), kept inside the window.
    readonly property var placement: chromeStyle.palettePlacement(root.window ? root.window.width : 0,
                                                                  root.window ? root.window.height : 0)
    readonly property var hangingAt: root.anchorRect
        ? chromeStyle.popupBelow(root.anchorRect, root.width, root.height,
                                 root.window ? root.window.width : 0, root.window ? root.window.height : 0)
        : null

    modal: true
    focus: true
    padding: 0
    // Not CloseOnEscape: what Escape does is the owner's (back one level, clear the field, close), and the
    // Popup's own Escape shortcut would take the key first and close the picker instead.
    closePolicy: Popup.CloseOnPressOutside

    parent: Overlay.overlay
    x: root.hangingAt ? root.hangingAt.x : root.placement.x
    y: root.hangingAt ? root.hangingAt.y : root.placement.y
    width: root.placement.width
    // Fits its content, up to the placement's height.
    height: Math.min(root.placement.maxHeight, root.fixedHeight + listView.contentHeight)

    // Everything in the column but the list: the query row, the two rules, the empty label and the footer.
    readonly property real fixedHeight: queryRow.height + topRule.height + empty.height + bottomRule.height
                                        + footer.height

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
            objectName: root.namePrefix + "EnterFade"
            property: "opacity"
            from: 0
            to: 1
            duration: chromeStyle.motion.standard
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            objectName: root.namePrefix + "EnterScale"
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
            objectName: root.namePrefix + "ExitFade"
            property: "opacity"
            to: 0
            duration: chromeStyle.motion.fast
            easing.type: Easing.InCubic
        }
    }

    // The exit transition's fade, for the tests: unlike the enter transition's, its animation is not a
    // QObject child of the popup, so findChild() cannot reach it.
    readonly property var _exitFade: exitFade

    // What had the keyboard when the picker opened. A dialog or popup (the find bar, a flyout, About) gets it
    // back on close; anything else is the terminal, which restoreTerminalFocus() reaches.
    property Item _returnFocus: null

    // What closeThen() was given, held until this popup has actually closed -- see onClosed.
    property var _pending: null

    // Refresh + focus on every open, not just the first: the owner rebuilds its rows (open tabs change), and
    // the filter must start empty with the caret already in it. Here, as the popup starts to show, and not in
    // onOpened, which waits out the enter transition: keys typed at once would otherwise be lost. What had the
    // keyboard is captured first, before the field takes it.
    //
    // A pick still pending is dropped: reopened during its exit transition, the popup cancels that exit without
    // closed() (QQuickPopup's transitionEnter), so the pick was never run, and must not run on the next close.
    onAboutToShow: {
        root._pending = null;
        root.opening();
        listHover.hasPosition = false;
        root._returnFocus = root.window ? root.window.activeFocusItem : null;
        filterField.clear();
        filterField.forceActiveFocus();
        root.select(listView.count > 0 ? 0 : -1);
    }

    // Where the keyboard goes back to on close: @p item, or where it has been hidden (a dialog's button gone
    // while the picker was up) its nearest visible ancestor in the same popup or dialog. Null when there is
    // none -- @p item was not over the overlay, or its popup has closed -- and the terminal takes it.
    // One walk up: the first visible node is the candidate, and it counts only if the walk reaches the
    // overlay, i.e. @p item sits inside a popup or a dialog.
    function _handBackTarget(item) {
        const overlay = root.Overlay.overlay;
        let target = null;
        for (let node = item; node; node = node.parent) {
            if (node === overlay)
                return target;
            if (!target && node.visible)
                target = node;
        }
        return null;
    }

    // Whatever closed the picker -- Escape, a pick, a click outside -- the keyboard must go back where it was:
    // to the dialog or popup the picker opened over, else to the terminal, or the user is left typing into
    // nothing. Only THEN does the pick run: a command that opens a keyboard-driven surface of its own
    // (SetTabColor's swatch picker, SetTabTitle's rename field) takes the focus as it opens, and running it any
    // earlier means this restore takes that focus straight back. Same law, and the same shape, as
    // ActionContextMenu.qml's _pendingActionId: act once the popup is genuinely gone.
    //
    // Qt.callLater, not a direct call: "genuinely gone" is not yet true INSIDE this handler. Qt emits closed()
    // from within the popup's own exit transition, so a pick that re-opens the picker (OpenCommandPalette, a
    // row the palette itself always offers) would re-enter Popup.open() on top of the close that is still
    // unwinding, and the picker would need dismissing twice. Deferring to the next event-loop turn dispatches
    // with the popup's state machine at rest.
    onClosed: {
        const target = root._handBackTarget(root._returnFocus);
        if (target)
            target.forceActiveFocus();
        else if (root.window)
            root.window.restoreTerminalFocus();
        root._returnFocus = null;
        const payload = root._pending;
        root._pending = null; // cleared BEFORE dispatch: the pick may re-open this picker
        if (payload !== null)
            Qt.callLater(root._dispatch, payload);
    }

    // The deferred half of onClosed.
    function _dispatch(payload) {
        root.picked(payload);
    }

    /// Closes the picker, then -- once it has closed and the keyboard is back -- reports @p payload as picked.
    function closeThen(payload) {
        root._pending = payload;
        root.close();
    }

    /// Moves the selection by @p delta rows, within the list.
    function step(delta) {
        if (listView.count === 0)
            return;
        root.select(Math.max(0, Math.min(listView.currentIndex + delta, listView.count - 1)));
        root.stepped(delta);
    }

    /// Selects @p index and keeps it in view -- the arrow keys walk a 200-row level past the viewport.
    function select(index) {
        listView.currentIndex = index;
        if (index >= 0)
            listView.positionViewAtIndex(index, ListView.Contain);
    }

    contentItem: Column {
        spacing: 0

        // {{{ The query row: the prompt glyph, the level chip, the field
        Item {
            id: queryRow
            objectName: root.namePrefix + "Query"
            width: parent.width
            height: chromeStyle.size.paletteField

            Row {
                id: queryLead
                // The query starts where the rows' titles do; the prompt and the level chip sit apart from it
                // and from each other by s4 (one cell on the Terminal row, where s2 is none).
                x: root.textInset
                anchors.verticalCenter: parent.verticalCenter
                spacing: chromeStyle.space.s4

                Label {
                    objectName: root.namePrefix + "Prompt"
                    visible: chromeStyle.palettePromptGlyph !== ""
                    text: chromeStyle.palettePromptGlyph
                    font: chromeStyle.type.paletteInput
                    color: chromeStyle.colors.paletteSecondaryText
                    Accessible.ignored: true
                }

                Label {
                    objectName: root.namePrefix + "Level"
                    visible: root.levelTitle !== ""
                    text: root.levelTitle !== "" ? root.levelTitle + " " + chromeStyle.drillInGlyph : ""
                    font: chromeStyle.type.paletteInput
                    color: chromeStyle.colors.paletteSecondaryText
                }
            }

            TextField {
                id: filterField
                objectName: root.namePrefix + "Filter"
                x: queryLead.x + (queryLead.width > 0 ? queryLead.width + queryLead.spacing : 0)
                width: parent.width - x - root.textInset
                anchors.verticalCenter: parent.verticalCenter
                // The one editable node assistive technology sees.
                Accessible.role: Accessible.EditableText
                Accessible.name: root.fieldName
                font: chromeStyle.type.paletteInput
                color: chromeStyle.colors.paletteText
                // The picker's surface is the field's: no frame of its own.
                background: Item {}
                padding: 0

                // The hint, drawn here rather than left to the style as placeholderText: every style draws that
                // from where the first letter will be typed, which is where the caret stands, so the caret stood
                // on the hint's first letter. This one starts past the caret, by a hairline.
                Label {
                    id: filterHint
                    objectName: root.namePrefix + "Hint"
                    x: filterField.cursorRectangle.x + filterField.cursorRectangle.width + chromeStyle.borderWidth
                    width: filterField.width - filterHint.x - filterField.rightPadding
                    anchors.verticalCenter: parent.verticalCenter
                    visible: filterField.length === 0 && filterField.preeditText === ""
                    text: root.hintText
                    font: filterField.font
                    color: chromeStyle.colors.paletteSecondaryText
                    elide: Text.ElideRight
                    Accessible.ignored: true
                }

                // The owner re-ranks (and regroups) on every keystroke; the best match is selected.
                onTextChanged: {
                    root.queryEdited(text);
                    root.select(listView.count > 0 ? 0 : -1);
                }

                // Every key is the owner's to decide first: the arrows move the SELECTION while the caret stays
                // in the field, so the user never has to leave the box to pick a row.
                Keys.onPressed: (event) => root.keyPressed(event)
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
            id: listView
            objectName: root.namePrefix + "List"
            width: parent.width
            // Takes whatever the query row, the rules and the footer leave.
            height: Math.max(0, root.height - root.fixedHeight)
            clip: true
            currentIndex: 0
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar {}

            // Only a real move counts: after every model reset Qt sends a synthetic hover at the last pointer
            // position, which would otherwise read as the pointer arriving at whatever row is now under it.
            HoverHandler {
                id: listHover
                property point lastPosition: Qt.point(-1, -1) // in the window's coordinates
                // False until the first event since the pointer entered the list (or the picker opened): that
                // event is where the pointer already was -- a picker opening under a resting pointer -- so it
                // only records the position.
                property bool hasPosition: false
                onHoveredChanged: if (!listHover.hovered) listHover.hasPosition = false
                onPointChanged: {
                    // Moved is judged in the window's coordinates: the open transition scales the list under a
                    // resting pointer, which moves it in the list's own.
                    const scene = listHover.point.scenePosition;
                    if (!listHover.hasPosition) {
                        listHover.hasPosition = true;
                        listHover.lastPosition = Qt.point(scene.x, scene.y);
                        return;
                    }
                    if (scene.x === listHover.lastPosition.x && scene.y === listHover.lastPosition.y)
                        return;
                    listHover.lastPosition = Qt.point(scene.x, scene.y);
                    const position = listHover.point.position;
                    const index = listView.indexAt(position.x + listView.contentX, position.y + listView.contentY);
                    const item = index >= 0 ? listView.itemAtIndex(index) : null;
                    // Over a group header, no row: only the row below it is selectable.
                    const overRow = item && position.y + listView.contentY >= item.y + item.headerHeight;
                    root.pointerMoved(overRow ? index : -1);
                }
            }
        }

        Label {
            id: empty
            objectName: root.namePrefix + "Empty"
            visible: listView.count === 0
            width: parent.width
            height: visible ? chromeStyle.size.paletteRow : 0
            leftPadding: root.textInset
            verticalAlignment: Text.AlignVCenter
            text: root.emptyText
            font: chromeStyle.type.body
            color: chromeStyle.colors.paletteSecondaryText
        }
        // }}}

        // {{{ The footer: the selected row's description, and its keys.
        Rectangle {
            id: bottomRule
            width: parent.width
            height: chromeStyle.borderWidth
            color: chromeStyle.colors.paletteSeparator
        }

        Item {
            id: footer
            objectName: root.namePrefix + "Footer"
            width: parent.width
            height: chromeStyle.size.paletteFooter

            Label {
                id: footerHintsLabel
                objectName: root.namePrefix + "FooterHints"
                x: parent.width - width - root.textInset
                anchors.verticalCenter: parent.verticalCenter
                text: root.footerHints
                font: chromeStyle.type.footnote
                color: chromeStyle.colors.paletteSecondaryText
            }

            Label {
                objectName: root.namePrefix + "FooterDescription"
                x: root.textInset
                width: Math.max(0, footerHintsLabel.x - chromeStyle.space.s4 - x)
                anchors.verticalCenter: parent.verticalCenter
                text: root.footerDescription
                horizontalAlignment: Text.AlignLeft
                elide: Text.ElideRight
                font: chromeStyle.type.footnote
                color: chromeStyle.colors.paletteSecondaryText
            }
        }
        // }}}
    }
}
