// vim:syntax=qml
// The find bar: a GUI-native search widget floating over one terminal pane.
//
// Opened by the SearchReverse action (Ctrl+Shift+F by default) and by `/` in Vi normal mode, both of
// which reach vtbackend's Terminal::Events::searchPromptRequested and arrive here as the session's
// searchBarRequested signal. Search used to be typed into the indicator status line instead; see
// vtbackend/screen/StatusLineBuilder.cpp for what {SearchPrompt} still shows.
//
// It belongs to the PANE, not to the window, because each pane is a session with its own scrollback
// and therefore its own pattern, its own match count and its own highlights. That is why it lives in
// SessionChrome (which already re-targets on every session rebind) rather than beside the command
// palette in Main.qml.
//
// Everything visible is a Qt Quick Control, so `ui_style: terminal` restyles the whole bar through
// styles/ContourTui/ without a single branch in this file -- square corners, whole character cells and
// the terminal's own font. That is the answer to the "TUI-hardliner" half of issue #2081.
//
// NB: never run clang-format over this file. It does not know QML, and silently reflows it into
// something the QML compiler rejects.
import QtQuick
import QtQuick.Controls

Popup {
    id: root

    // The TerminalSession. Guarded EVERYWHERE below against BOTH null and undefined: a split pane's
    // session is rebound to null during teardown, and several offscreen tests bind SessionChrome to a
    // lightweight MOCK session carrying none of these properties. Assigning undefined to a TYPED
    // property is a QML warning, which the run-wide diagnostic gate in test_main.cpp turns into a
    // failure of the entire suite. Same rule, same reason, as SessionChrome's hyperlink tooltip.
    required property var session

    // The ContourTerminal this bar floats over; also who gets the keyboard back when it closes.
    required property var displayItem

    // Modeless, and deliberately so: the terminal keeps rendering, output keeps arriving, and clicking
    // into the grid hands the keyboard back WITHOUT dismissing the bar -- which is what NoAutoClose
    // buys. Every other popup in this application is modal; this is the first that is not. Escape is
    // handled explicitly on the field, since CloseOnEscape would also fire while the terminal has
    // focus and the user is only trying to leave Vi mode.
    modal: false
    focus: true
    padding: chromeStyle.borderWidth
    closePolicy: Popup.NoAutoClose

    // Docked to the pane's top-right, where VS Code and Windows Terminal both put it. Bound rather
    // than assigned once, so it follows a pane resize or a split drag.
    parent: root.displayItem
    x: root.displayItem ? root.displayItem.width - width - chromeStyle.shadowMargin : 0
    y: chromeStyle.shadowMargin
    // The backstop for a pane narrower than the bar, where the x above goes negative and would clip
    // the field off the left edge. TabColorFlyout documents the same rule.
    margins: chromeStyle.shadowMargin

    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Mandatory, not decorative: `ui_style` defaults to Native, which pins Fusion -- a Qt style whose
    // control files are not ours to edit -- so a background left to the style would be unshadowed and
    // differently shaped from every other popup here. See PopupSurface.qml.
    background: PopupSurface {}

    // One guarded alias rather than the same test repeated at every use: null while a split pane is
    // being torn down, and null for the lightweight MOCK sessions several offscreen tests bind, which
    // carry none of these properties. Assigning undefined to a TYPED property is a QML warning, and
    // the run-wide diagnostic gate in test_main.cpp fails the entire suite on one.
    readonly property var _search: (root.session && root.session.searchSummary !== undefined)
                                   ? root.session : null

    readonly property bool _hasMatches: root._search ? root._search.searchHasMatches : true
    readonly property bool _navigable: root._search ? root._search.searchNavigable : false

    // The glyph and the lit state come from the session already resolved, rather than as an
    // enumerator this file would have to interpret. It spoke raw integers once and inverted every one
    // of them -- the glyph, both tooltips and the cycle order. @see describeSearchCase.
    readonly property string _caseGlyph: root._search ? root._search.searchCaseGlyph : "Aa"
    readonly property bool _casePinned: root._search ? root._search.searchCasePinned : false
    readonly property string _caseTooltip: root._search ? root._search.searchCaseTooltip : ""

    // The tint a fruitless search takes. From the style's own token row rather than a literal here,
    // so the two chrome styles can disagree about it like they do about every other colour value.
    readonly property color errorColor: chromeStyle.errorColor

    // Which session was last told this bar is open, which is emphatically NOT "whatever session is
    // bound right now": the bar OUTLIVES a rebind, because SessionChrome re-targets on every tab
    // switch and pane hand-off while the popup stays open. Told-ness therefore has to be HANDED OVER.
    // Without that, the session left behind kept _isSearchBarOpen set and re-armed a walk of its whole
    // scrollback every 250 ms for the rest of its life, and the session arriving was never told at all,
    // so its count never appeared.
    property var _notifiedSession: null

    // Brings the session's told-ness in line with @p target, which is the bound session while the bar
    // is open and null otherwise. Idempotent, so every caller can just state the intent.
    function _syncOpenState(target) {
        if (target === root._notifiedSession)
            return;
        if (root._notifiedSession)
            root._notifiedSession.searchBarClosed();
        root._notifiedSession = target;
        // Tallying costs a walk of the whole scrollback, so the session only does it while something
        // is displaying the result. It cannot infer that; only this bar knows.
        if (target)
            target.searchBarOpened();
    }

    // A pane torn down with the bar still standing open emits no closed(), so the session it last told
    // would keep re-arming a walk of its whole scrollback -- and, since screenUpdated() reads the same
    // flag, keep the viewport pinned off the bottom -- for the rest of its life. The handover rule once
    // more, with null as the target.
    Component.onDestruction: root._syncOpenState(null)

    // True while the field is being re-seeded FROM the session, which is not the user typing. The
    // field's onTextChanged pushes what it holds back into the session, so without this a seed bounces
    // straight back as a setSearchPattern() -- another walk of the whole scrollback, and a re-run of
    // the search that moves the very cursor the seed was there to preserve.
    property bool _seeding: false

    // Shows @p text in the field without pushing it back into the session. QML property writes notify
    // synchronously, so the flag need only span the assignment.
    function _seedField(text) {
        root._seeding = true;
        field.text = text;
        root._seeding = false;
    }

    // Takes @p target on as the session the OPEN bar belongs to: hand the told-ness over, then show
    // that session's own term. The two always go together -- a bar displaying one session's term while
    // another is being told it is open is precisely the state the handover exists to prevent -- so both
    // the initial open and a rebind under an open bar go through here rather than repeating the pair.
    function _adoptSession(target) {
        root._syncOpenState(target);
        root._seedField(target ? target.searchPattern : "");
    }

    // Opening onto an existing pattern selects it, so typing replaces rather than appends -- the
    // behaviour every find bar has, and what makes a second Ctrl+Shift+F a "search for something else"
    // rather than a "keep typing".
    onOpened: {
        root._adoptSession(root._search);
        root.focusField();
    }

    // Whatever closed the bar, the terminal must get the keyboard back, or the user is left typing
    // into nothing. Same law, and the same reason, as CommandPalette.qml states at length.
    onClosed: {
        root._syncOpenState(null);
        if (root.displayItem)
            root.displayItem.forceActiveFocus();
    }

    // The pane was handed a different session while the bar stood open. Hand the told-ness over, then
    // re-seed the field from the session that owns it now -- the term on screen belongs to the
    // scrollback being searched, and the outgoing one's would otherwise be typed into the incoming one.
    on_SearchChanged: {
        if (root.visible)
            root._adoptSession(root._search);
        else
            root._syncOpenState(null);
    }

    // Re-focusing an already-open bar, for a second Ctrl+Shift+F.
    function focusField() {
        field.forceActiveFocus();
        field.selectAll();
    }

    // Three states rather than VS Code's two, because issue #1410 asks for smart case AND a way to
    // override it, and two cannot express three. WHICH three, and in what order, is decided in C++
    // where the enum is -- `(mode + 1) % 3` here hard-coded a count this file cannot see.
    function cycleCase() {
        if (root._search)
            root._search.cycleSearchCaseSensitivity();
    }

    // The search can be cleared from outside this bar -- Ctrl+Shift+H, or entering Vi Insert mode --
    // and the terminal then holds no pattern while the field still shows one. Resync only in that
    // direction: the field is the source of truth while the user is typing into it.
    Connections {
        target: root._search
        ignoreUnknownSignals: true
        function onSearchStateChanged() {
            if (root._search && root._search.searchPattern === "" && field.text !== "")
                root._seedField("");
        }
    }

    // Enter / Shift+Enter. Split out so both Return and Enter (the keypad one) reach the same rule.
    function step(event) {
        if (!root._search)
            return;
        if (event.modifiers & Qt.ShiftModifier)
            root._search.searchPrevious();
        else
            root._search.searchNext();
        event.accepted = true;
    }

    contentItem: Row {
        id: bar
        spacing: chromeStyle.labelGap

        // The field handles Escape itself so the caret case is direct; this catches the same key when
        // focus has tabbed onto one of the buttons, where an unhandled Escape would otherwise leave the
        // only modeless popup in the application with no way out but the mouse.
        Keys.onEscapePressed: root.close();

        // {{{ The term, with its count inside the field — where VS Code puts it, and what keeps the
        // bar from growing a separate column that shifts every time the count changes width.
        TextField {
            id: field
            objectName: "searchBarField"
            anchors.verticalCenter: parent.verticalCenter
            width: chromeStyle.cellWidth * 26
            placeholderText: qsTr("Find")
            // Room for the count that floats over the field's right end, so a long term scrolls
            // rather than running underneath "9999+ matches".
            rightPadding: count.width + 2 * chromeStyle.labelPadding

            // Both chrome styles draw a focused field's edge in the Highlight role -- ContourTui says
            // so outright, and Fusion's focus frame uses it too -- so re-pointing that one role is how
            // a fruitless search reddens the edge in EITHER style, without this file replacing a
            // background and losing the styling the whole design rests on.
            palette.highlight: root._hasMatches ? systemPalette.highlight : root.errorColor

            // Live as the user types: this is an incremental search, so each keystroke re-runs it and
            // moves the viewport onto the nearest match behind the cursor.
            onTextChanged: if (!root._seeding && root._search) root._search.setSearchPattern(text);

            Keys.onEscapePressed: root.close();

            // Enter steps forward, Shift+Enter back. Handled here rather than as a Popup-level
            // shortcut so the caret stays in the field the whole time.
            Keys.onReturnPressed: (event) => root.step(event);
            Keys.onEnterPressed: (event) => root.step(event);

            Label {
                id: count
                objectName: "searchBarCount"
                anchors.right: parent.right
                anchors.rightMargin: chromeStyle.labelPadding
                anchors.verticalCenter: parent.verticalCenter
                text: root._search ? root._search.searchSummary : ""
                // Dimmed against the term: it reports on the search rather than being part of it.
                // Turns to the error color when the term matches nothing, which is the same signal the
                // field's own edge carries -- stated twice on purpose, since the edge is a hairline.
                color: root._hasMatches
                       ? Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g,
                                 systemPalette.windowText.b, 0.62)
                       : root.errorColor
            }
        }
        // }}}

        ToolButton {
            objectName: "searchBarCaseToggle"
            anchors.verticalCenter: parent.verticalCenter
            text: root._caseGlyph
            // Deliberately NOT `checkable`. A checkable button toggles `checked` itself on click, and
            // that write only gets corrected when the binding's value actually changes -- so cycling
            // Sensitive -> Insensitive, where pinned stays true, left the button unlit while the policy
            // was pinned. Three states cannot be driven by a two-state toggle; the model is the only
            // one that knows, so `checked` stays a pure binding and the click only asks for the cycle.
            //
            // Lit means "you pinned this"; unlit is smart case, which is what the terminal has always
            // done and what an untouched bar opens with.
            checked: root._casePinned
            onClicked: root.cycleCase()
            ToolTip.visible: hovered
            ToolTip.text: root._caseTooltip
        }

        // The style's own rule where it has one, and nothing where it does not: ContourTui draws a
        // box-drawing separator between chrome groups, Native leaves them abutting.
        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: chromeStyle.tabSeparator
            visible: text.length > 0
            color: systemPalette.mid
        }

        ToolButton {
            objectName: "searchBarPrevious"
            anchors.verticalCenter: parent.verticalCenter
            text: "▴"
            enabled: root._navigable
            onClicked: if (root._search) root._search.searchPrevious();
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Previous match — Shift+Enter")
        }

        ToolButton {
            objectName: "searchBarNext"
            anchors.verticalCenter: parent.verticalCenter
            text: "▾"
            enabled: root._navigable
            onClicked: if (root._search) root._search.searchNext();
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Next match — Enter")
        }

        ToolButton {
            objectName: "searchBarClose"
            anchors.verticalCenter: parent.verticalCenter
            // The style's own close affordance, so this bar closes with the same glyph as every tab.
            text: chromeStyle.closeGlyph
            onClicked: root.close()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Close — Esc")
        }
    }
}
