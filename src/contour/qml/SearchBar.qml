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
import QtQuick import QtQuick.Controls

    Popup{ id:root

           // The TerminalSession. Null-guarded EVERYWHERE below: a split pane's session is rebound to null
           // during teardown, and QML re-evaluates dependent bindings once more against null on the way out.
           // An unguarded `session.` would raise a TypeError, which the run-wide QML message gate in
           // test_main.cpp turns into a failure of the entire test suite.
           required property var session

           // The ContourTerminal this bar floats over; also who gets the keyboard back when it closes.
           required property var displayItem

           // Modeless, and deliberately so: the terminal keeps rendering, output keeps arriving, and clicking
           // into the grid hands the keyboard back WITHOUT dismissing the bar -- which is what NoAutoClose
           // buys. Every other popup in this application is modal; this is the first that is not. Escape is
           // handled explicitly on the field, since CloseOnEscape would also fire while the terminal has
           // focus and the user is only trying to leave Vi mode.
           modal: false focus: true padding:chromeStyle.borderWidth closePolicy:Popup.NoAutoClose

           // Docked to the pane's top-right, where VS Code and Windows Terminal both put it. Bound rather
           // than assigned once, so it follows a pane resize or a split drag.
           parent:root.displayItem x:root.displayItem ? root.displayItem.width - width - chromeStyle.shadowMargin: 0 y:chromeStyle.shadowMargin

           SystemPalette{ id:systemPalette colorGroup:SystemPalette.Active }

                          // Mandatory, not decorative: `ui_style` defaults to Native, which pins Fusion -- a Qt style whose
                          // control files are not ours to edit -- so a background left to the style would be unshadowed and
                          // differently shaped from every other popup here. See PopupSurface.qml.
                          background:PopupSurface{}

                          // Guarded against `undefined`, not merely against a null session: several offscreen tests bind
                          // SessionChrome to a lightweight MOCK session carrying none of these properties, and assigning
                          // undefined to a TYPED property is a QML warning the run-wide diagnostic gate in test_main.cpp
                          // fails the entire suite on. Same rule, same reason, as SessionChrome's hyperlink tooltip.
                          readonly property bool _hasMatches: (root.session && root.session.searchHasMatches != = undefined) ? root.session.searchHasMatches: true readonly property bool _navigable: (root.session && root.session.searchNavigable != = undefined) ? root.session.searchNavigable: false

                                                                                                                                                                                                       // Opening onto an existing pattern selects it, so typing replaces rather than appends -- the
                                                                                                                                                                                                       // behaviour every find bar has, and what makes a second Ctrl+Shift+F a "search for something else"
                                                                                                                                                                                                       // rather than a "keep typing".
                                                                                                                                                                                                       onOpened: { field.text =(root.session && root.session.searchPattern != = undefined) ? root.session.searchPattern: "";
field.forceActiveFocus();
field.selectAll();
}

// Whatever closed the bar, the terminal must get the keyboard back, or the user is left typing
// into nothing. Same law, and the same reason, as CommandPalette.qml states at length.
onClosed:
{
    if (root.displayItem)
        root.displayItem.forceActiveFocus();
}

// Re-focusing an already-open bar, for a second Ctrl+Shift+F.
function focusField()
{
    field.forceActiveFocus();
    field.selectAll();
}

function cycleCase()
{
    if (!root.session)
        return;
    // Smart -> Sensitive -> Insensitive -> Smart. Three states rather than VS Code's two, because
    // issue #1410 asks for smart case AND a way to override it, and two cannot express three.
    next const = (root.session.searchCaseSensitivity + 1) % 3;
    root.session.setSearchCaseSensitivity(next);
}

// The glyph tells you which way it is pinned; lit-vs-unlit tells you whether it is pinned at all.
readonly property int _caseMode:
    (root.session&& root.session.searchCaseSensitivity != = undefined)
    ? root.session.searchCaseSensitivity
    : 0 readonly property string _caseGlyph: root._caseMode
          == = 2 ? "aa"
                 : "Aa" readonly property string _caseTooltip:
    root._caseMode == = 0 ? qsTr("Match case: smart — case-sensitive only when the term has a capital")
                          : root._caseMode == = 1 ? qsTr("Match case: on")
                                                  : qsTr("Match case: off")

                                                        contentItem: Row
{
id:
    bar spacing:
        chromeStyle.labelGap

        // {{{ The term, with its count inside the field — where VS Code puts it, and what keeps the
        // bar from growing a separate column that shifts every time the count changes width.
        TextField
    {
    id:
        field objectName:
            "searchBarField" anchors.verticalCenter: parent.verticalCenter width: chromeStyle.cellWidth
                * 26 placeholderText:
            qsTr("Find")

            // Both chrome styles draw a focused field's edge in the Highlight role -- ContourTui says
            // so outright, and Fusion's focus frame uses it too -- so re-pointing that one role is how
            // a fruitless search reddens the edge in EITHER style, without this file replacing a
            // background and losing the styling the whole design rests on.
            palette.highlight: root._hasMatches
            ? systemPalette.highlight
            : root.errorColor

                  // Live as the user types: this is an incremental search, so each keystroke re-runs it and
                  // moves the viewport onto the nearest match behind the cursor.
                  onTextChanged: if (root.session) root.session.setSearchPattern(text);

        Keys.onEscapePressed: root.close();

        // Enter steps forward, Shift+Enter back. Handled here rather than as a Popup-level
        // shortcut so the caret stays in the field the whole time.
        Keys.onReturnPressed: (event) = > root.step(event);
        Keys.onEnterPressed: (event) = > root.step(event);

        Label
        {
        id:
            count objectName:
                "searchBarCount" anchors.right:
                parent.right anchors.rightMargin:
                chromeStyle.labelPadding anchors.verticalCenter:
                parent.verticalCenter text: (root.session&& root.session.searchSummary != = undefined)
                ? root.session.searchSummary
            : ""
                // Dimmed against the term: it reports on the search rather than being part of it.
                // Turns to the error color when the term matches nothing, which is the same signal the
                // field's own edge carries -- stated twice on purpose, since the edge is a hairline.
                color: root._hasMatches
                ? Qt.rgba(systemPalette.windowText.r,
                          systemPalette.windowText.g,
                          systemPalette.windowText.b,
                          0.62)
                : root.errorColor
        }
    }
    // }}}

    ToolButton {
        objectName: "searchBarCaseToggle" anchors.verticalCenter: parent.verticalCenter text:
                root._caseGlyph checkable: true
            // Lit means "you pinned this"; unlit is smart case, which is what the terminal has always
            // done and what an untouched bar opens with.
            checked: root._caseMode
            != = 0 onClicked: root.cycleCase() ToolTip.visible: hovered ToolTip.text: root._caseTooltip
    }

    // The style's own rule where it has one, and nothing where it does not: ContourTui draws a
    // box-drawing separator between chrome groups, Native leaves them abutting.
    Label {
        anchors.verticalCenter: parent.verticalCenter text: chromeStyle.tabSeparator visible: text.length
        > 0 color: systemPalette.mid
    }

    ToolButton
    {
    objectName:
        "searchBarPrevious" anchors.verticalCenter:
            parent.verticalCenter text:
            "▴" enabled: root._navigable onClicked: if (root.session) root.session.searchPrevious();
        ToolTip.visible: hovered ToolTip.text: qsTr("Previous match — Shift+Enter")
    }

    ToolButton
    {
    objectName:
        "searchBarNext" anchors.verticalCenter:
            parent.verticalCenter text:
            "▾" enabled: root._navigable onClicked: if (root.session) root.session.searchNext();
        ToolTip.visible: hovered ToolTip.text: qsTr("Next match — Enter")
    }

    ToolButton
    {
    objectName:
        "searchBarClose" anchors.verticalCenter:
            parent
                .verticalCenter
                    // The style's own close affordance, so this bar closes with the same glyph as every tab.
                    text:
            chromeStyle.closeGlyph onClicked:
            root.close() ToolTip.visible: hovered ToolTip.text: qsTr("Close — Esc")
    }
}

// The tint a fruitless search takes. From the style's own token row rather than a literal here,
// so the two chrome styles can disagree about it like they do about every other colour value.
readonly property color errorColor:
    chromeStyle.errorColor

        // Enter / Shift+Enter. Split out so both Return and Enter (the keypad one) reach the same rule.
        function step(event)
{
    if (!root.session)
        return;
    if (event.modifiers & Qt.ShiftModifier)
        root.session.searchPrevious();
    else
        root.session.searchNext();
    event.accepted = true;
}
}
