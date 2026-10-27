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
import QtQuick.Layouts

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
    padding: 0
    closePolicy: Popup.NoAutoClose

    objectName: "searchBar"

    // Whether the pane shows an inline header above its terminal (a window::PaneHeaderPresence's value).
    property int header: 0
    // The pane header's find slot (PaneHeader's findSlot), which the Terminal row's bar takes in place of
    // a grid row when placeFindBar anchors it to "headerRow"; null where the pane draws no header.
    property Item headerSlot: null

    // The current match's band in the terminal, where the bar would cover it: the session names its rows,
    // the terminal says where they are. Empty -- top and bottom 0 -- while there is none.
    readonly property var _rows: (root._search && root._search.searchFocusedRows) ? root._search.searchFocusedRows : ({})
    readonly property var _band: (root._rows.first !== undefined && root.displayItem
                                  && typeof root.displayItem.rowBand === "function")
                                 ? root.displayItem.rowBand(root._rows.first, root._rows.last)
                                 : ({ top: 0, bottom: 0 })

    // Where the bar goes, decided once in C++ (window::placeFindBar): top right, under a header, or out of
    // the current match's way at the bottom right. Bound, so it follows a resize, a split drag or a step.
    readonly property var placement: root.displayItem
                                     ? chromeStyle.findBarPlacement(root.header, root.displayItem.width,
                                                                    root.displayItem.height,
                                                                    root._band.top, root._band.bottom)
                                     : ({ anchor: "topRight", x: 0, y: 0, width: 0 })

    // The Terminal row's bar takes the pane header's row where the pane has one: the header's own fill
    // behind it, no grid row spent on it.
    // Both stay bool (AGENT.md's QML-facing carve-out): each is a predicate the reader asks of the bar --
    // is it in the header row? -- derived from placement.anchor, which is where the named outcome lives;
    // PaneHeader reads occupiesHeaderRow through SessionChrome and TerminalPane to give the bar its row.
    readonly property bool _inHeaderRow: root.placement.anchor === "headerRow" && root.headerSlot !== null
    readonly property bool occupiesHeaderRow: root._inHeaderRow && root.visible

    parent: root._inHeaderRow ? root.headerSlot : root.displayItem
    x: root._inHeaderRow ? 0 : root.placement.x
    y: root._inHeaderRow ? 0 : root.placement.y
    width: root._inHeaderRow ? root.headerSlot.width : root.placement.width
    height: root._inHeaderRow ? root.headerSlot.height : chromeStyle.size.findBar
    // None: the placement already keeps the bar inside its pane, and a margin would clamp it off the
    // design's place (F7). The shadow is drawn outside the surface, past the popup's own bounds.
    margins: 0

    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Mandatory, not decorative: `ui_style` defaults to Native, which pins Fusion -- a Qt style whose
    // control files are not ours to edit -- so a background left to the style would be unshadowed and
    // differently shaped from every other popup here. See PopupSurface.qml.
    background: PopupSurface {
        objectName: "searchBarBackground"
        radius: chromeStyle.radii.findBar
        visible: !root._inHeaderRow
    }

    // One guarded alias rather than the same test repeated at every use: null while a split pane is
    // being torn down, and null for the lightweight MOCK sessions several offscreen tests bind, which
    // carry none of these properties. Assigning undefined to a TYPED property is a QML warning, and
    // the run-wide diagnostic gate in test_main.cpp fails the entire suite on one.
    readonly property var _search: (root.session && root.session.searchSummary !== undefined)
                                   ? root.session : null

    readonly property bool _hasMatches: root._search ? root._search.searchHasMatches : true
    readonly property bool _navigable: root._search ? root._search.searchNavigable : false

    // The tint a fruitless search or a refused pattern takes: the Danger status token
    // (UiStyleProvider::errorColor), the same on every chrome row, rather than a literal here.
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

    readonly property var _toggles: root._search ? root._search.searchToggles : []

    // The Terminal row lights a toggle in inverse video; the native rows wash it in the accent and ring it.
    readonly property bool _isCellRow: chromeStyle.layout.findBar === chromeStyle.layout.paneHeaderRow
    // The magnifier: a lens and its handle, as fractions of the icon's edge.
    readonly property real _lensFraction: 0.72
    readonly property real _handleFraction: 0.38
    readonly property real _half: 0.5

    contentItem: RowLayout {
        id: bar
        spacing: chromeStyle.space.s2

        // The field handles Escape itself so the caret case is direct; this catches the same key when
        // focus has tabbed onto one of the buttons, where an unhandled Escape would otherwise leave the
        // only modeless popup in the application with no way out but the mouse.
        Keys.onEscapePressed: root.close();

        // Leading padding, then the magnifier where the row draws one.
        Item {
            Layout.preferredWidth: chromeStyle.space.s5 - bar.spacing
            visible: Layout.preferredWidth > 0
        }

        Item {
            objectName: "searchBarIcon"
            visible: chromeStyle.size.findIcon > 0
            Layout.preferredWidth: chromeStyle.size.findIcon
            Layout.preferredHeight: chromeStyle.size.findIcon
            Layout.alignment: Qt.AlignVCenter

            Rectangle {
                id: lens
                width: parent.width * root._lensFraction
                height: width
                radius: width * root._half
                color: "transparent"
                border.width: chromeStyle.borderWidth
                border.color: chromeStyle.colors.findToggleText
            }
            Rectangle {
                width: parent.width * root._handleFraction
                height: chromeStyle.borderWidth
                color: chromeStyle.colors.findToggleText
                x: lens.width - width * root._half
                y: lens.height - height * root._half
                rotation: 45
            }
        }

        TextField {
            id: field
            objectName: "searchBarField"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            placeholderText: qsTr("Find")
            font: chromeStyle.type.body

            Accessible.role: Accessible.EditableText
            Accessible.name: qsTr("Find")

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
        }

        // The count, in tabular digits so "2 of 5" does not shift as it counts. In the error colour on a
        // miss or a refused pattern, the same signal the field's edge carries.
        Label {
            id: count
            objectName: "searchBarCount"
            Layout.alignment: Qt.AlignVCenter
            text: root._search ? root._search.searchSummary : ""
            font.family: chromeStyle.type.footnote.family
            font.pixelSize: chromeStyle.type.footnote.pixelSize
            font.weight: chromeStyle.type.footnote.weight
            font.features: { "tnum": 1 }
            color: root._hasMatches ? chromeStyle.colors.findCountText : root.errorColor
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        // The options, one per row of the session's table (SearchToggleRows): case, regex, whole word.
        // Modelled by COUNT, each button reading its row: the session publishes a new list on every change,
        // and a list model would rebuild every button -- dropping the keyboard focus a toggle press is
        // made with.
        Repeater {
            model: root._toggles.length

            delegate: ToolButton {
                id: toggle
                required property int index
                readonly property var modelData: root._toggles[index]
                objectName: modelData.objectName
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: chromeStyle.size.findToggleWidth
                Layout.preferredHeight: chromeStyle.size.findToggleHeight
                text: chromeStyle.chipOpenGlyph + modelData.glyph + chromeStyle.chipCloseGlyph
                font.family: chromeStyle.type.footnote.family
                font.pixelSize: chromeStyle.type.footnote.pixelSize
                // Deliberately NOT `checkable`. A checkable button toggles `checked` itself on click, and
                // that write is corrected only when the binding's value changes -- so cycling Sensitive ->
                // Insensitive, where the case toggle stays lit, left it unlit while the policy was pinned.
                // The session is the only one that knows; `checked` stays a pure binding.
                checked: modelData.checked
                onClicked: if (root._search) root._search.toggleSearchOption(modelData.toggle);
                ToolTip.visible: hovered
                ToolTip.text: modelData.tooltip

                Accessible.role: Accessible.Button
                Accessible.name: modelData.accessibleName
                Accessible.checkable: true
                Accessible.checked: modelData.checked

                contentItem: Label {
                    text: toggle.text
                    font: toggle.font
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: toggle.checked ? chromeStyle.colors.findToggleOnText : chromeStyle.colors.findToggleText
                }
                // A lit toggle stands off an unlit one at 3:1: the Terminal row's inverse cells, or the
                // native rows' accent wash with an accent ring (the chrome's ink where the accent is faint).
                background: Rectangle {
                    objectName: "searchBarToggleBackground"
                    radius: chromeStyle.radii.control
                    color: toggle.checked ? chromeStyle.colors.findToggleOnFill
                                          : (toggle.hovered ? chromeStyle.colors.tabHoverFill : "transparent")
                    border.width: toggle.checked && !root._isCellRow ? chromeStyle.borderWidth : 0
                    border.color: chromeStyle.statusMarkOn(chromeStyle.statusRunning, systemPalette.window)
                }
            }
        }

        ToolButton {
            objectName: "searchBarPrevious"
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: chromeStyle.size.findButtonWidth
            Layout.preferredHeight: chromeStyle.size.findButtonHeight
            text: chromeStyle.findPreviousGlyph
            enabled: root._navigable
            onClicked: if (root._search) root._search.searchPrevious();
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Previous match — Shift+Enter")
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Previous match")
        }

        ToolButton {
            objectName: "searchBarNext"
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: chromeStyle.size.findButtonWidth
            Layout.preferredHeight: chromeStyle.size.findButtonHeight
            text: chromeStyle.findNextGlyph
            enabled: root._navigable
            onClicked: if (root._search) root._search.searchNext();
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Next match — Enter")
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Next match")
        }

        ToolButton {
            objectName: "searchBarClose"
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: chromeStyle.size.findButtonWidth
            Layout.preferredHeight: chromeStyle.size.findButtonHeight
            // The style's own close affordance, so this bar closes with the same glyph as every tab.
            text: chromeStyle.closeGlyph
            onClicked: root.close()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Close — Esc")
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Close find")
        }

        // Trailing padding.
        Item {
            Layout.preferredWidth: chromeStyle.space.s2 - bar.spacing
            visible: Layout.preferredWidth > 0
        }
    }
}
