// vim:syntax=qml
// The kit's modal dialog: a heading, a message and an accept/reject pair on a PopupSurface.
//
// Built on QtQuick.Controls' in-scene Dialog, never QtQuick.Dialogs (see ConfirmDialog.qml). Where it
// appears is a token: a sheet hanging below the window's band when `layout.dialog` is `layout.sheet`,
// centred otherwise. Its width, insets, fonts and colours all come from the `chromeStyle` tokens, so this
// file never learns which style is active. It is never narrower than its widest row of controls -- the
// buttons, and whatever the body names in `bodyControlsWidth` -- and in whole cells where the chrome is.
//
// Keyboard: the modal popup keeps focus inside itself, Esc rejects it, and Enter takes the default button
// (the accept button, unless `defaultButton` names the reject one) -- never a disabled accept. The default
// button is the one drawn filled.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    objectName: "kitDialog"

    /// The bold heading line.
    property string heading
    /// The explanatory text under the heading.
    property string message
    /// The label on the accept button.
    property string acceptText: qsTr("OK")
    /// The label on the reject button.
    property string rejectText: qsTr("Cancel")
    /// The accept button's StatusColorRole; `chromeStyle.statusDanger` makes it destructive.
    property int acceptRole: chromeStyle.statusRunning
    /// Whether the accept button can be pressed; while false, Enter does nothing either.
    property bool acceptEnabled: true
    /// What a screen reader reads as the dialog's description: the message, unless the body says more.
    property string description: message
    /// What sits between the message and the buttons: a dialog's own controls (a field, a checkbox, lines).
    property alias bodyData: bodySlot.data
    /// The widest row of the body's controls (a checkbox's line), which the dialog grows to hold; 0 for none.
    property real bodyControlsWidth: 0

    /// Which button takes the initial focus and Enter, and is drawn filled.
    enum DefaultButton {
        Accept,
        Reject
    }
    /// A KitDialog.DefaultButton.
    property int defaultButton: KitDialog.Accept

    // The default that holds: the one asked for, unless it is the reject button and that is not shown (a dialog
    // with nothing to decline) -- then the one button there is.
    readonly property int _defaultRole: root.defaultButton === KitDialog.Reject && root.rejectText.length > 0
                                        ? KitDialog.Reject : KitDialog.Accept

    // The buttons, one row per role in the order they stand and take Tab: reject, then accept. A row says what
    // its button reads and does; how it is drawn follows from whether it is the default (_lookOf).
    readonly property var _buttonRows: [
        { role: KitDialog.Reject, name: "kitDialogReject", text: root.rejectText, statusRole: chromeStyle.statusRunning,
          enabled: true, shown: root.rejectText.length > 0, press: () => root.reject() },
        { role: KitDialog.Accept, name: "kitDialogAccept", text: root.acceptText, statusRole: root.acceptRole,
          enabled: root.acceptEnabled, shown: true, press: () => root.accept() }
    ]

    /// How a button is drawn.
    enum ButtonLook {
        Plain,       ///< The style's own button.
        Filled,      ///< The default: filled in its status colour (the accent for a reject button).
        RoleInWords  ///< Not the default, but a status role of its own (a destructive Close): the style's button,
                     ///< its label in that role's hue, as macOS writes a destructive button it does not make
                     ///< the default.
    }

    function _lookOf(row) {
        if (row.role === root._defaultRole)
            return KitDialog.Filled;
        return row.statusRole !== chromeStyle.statusRunning ? KitDialog.RoleInWords : KitDialog.Plain;
    }

    // The colour a default button is filled with: the accept role's (a destructive accept's danger), the accent
    // for a default reject button, as any other default button has it.
    function _fillRoleOf(row) {
        return row.role === KitDialog.Accept ? root.acceptRole : chromeStyle.statusRunning;
    }

    /// Escape was pressed while the dialog keeps it (its closePolicy has no CloseOnEscape): a dialog for
    /// which Escape means something else than its reject button answers it here.
    signal escaped()

    /// Takes the default button, as Enter does.
    function pressDefault() {
        if (root._defaultRole === KitDialog.Reject)
            root.reject();
        else if (root.acceptEnabled)
            root.accept();
    }

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: chromeStyle.space.s8

    // The token's width, grown to hold the widest row of controls, in whole width quanta (cells on the
    // Terminal row, pixels elsewhere) -- and never wider than what it hangs in, less a margin either side.
    readonly property real _controlsWidth: Math.max(buttonRow.implicitWidth, root.bodyControlsWidth)
    readonly property real _wantedWidth: Math.max(chromeStyle.size.dialogWidth,
                                                  root._controlsWidth + root.leftPadding + root.rightPadding)
    readonly property real _room: parent ? parent.width - 2 * chromeStyle.space.s8 : root._wantedWidth
    width: Math.min(chromeStyle.widthQuantum * Math.ceil(root._wantedWidth / chromeStyle.widthQuantum),
                    chromeStyle.widthQuantum * Math.floor(root._room / chromeStyle.widthQuantum))

    // The sheet hangs below the window's band and banners: from where the window says its content starts
    // (Main.qml's `sheetTop`), in the parent's coordinates, never above the parent's own top -- a dialog
    // parented to a pane hangs from the pane. Every other row centres.
    readonly property bool _isSheet: chromeStyle.layout.dialog === chromeStyle.layout.sheet
    readonly property var _window: parent ? parent.Window.window : null
    readonly property real _sheetTop: root._window && root._window.sheetTop !== undefined
                                      ? Math.max(0, parent.mapFromItem(null, 0, root._window.sheetTop).y)
                                      : 0
    x: parent ? (parent.width - width) / 2 : 0
    y: !parent ? 0 : _isSheet ? root._sheetTop : (parent.height - height) / 2

    // The text colours come from the application palette directly, as ConfirmDialog's do. The popup's
    // inherited palette is not enough: a binding to `root.palette.windowText` is taken before the popup
    // reaches its window and keeps the style's default (black) once the window's palette arrives, which on
    // a dark surface reads at under 2:1.
    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }

    background: PopupSurface {}

    // What every dialog button is, whatever its look: a row's words and answer, the footnote type. An inline
    // component sees no id of this file, so it reads the application palette (see `sys`) itself.
    component KitDialogButtonBase: Button {
        id: baseButton
        /// One of the dialog's _buttonRows.
        required property var row
        /// The application palette's button text, active and disabled.
        readonly property color activeInk: activePalette.buttonText
        readonly property color disabledInk: disabledPalette.buttonText
        objectName: baseButton.row.name
        text: baseButton.row.text
        enabled: baseButton.row.enabled
        focus: true
        font: chromeStyle.type.footnote
        Accessible.name: text
        onClicked: baseButton.row.press()

        SystemPalette {
            id: activePalette
            colorGroup: SystemPalette.Active
        }
        SystemPalette {
            id: disabledPalette
            colorGroup: SystemPalette.Disabled
        }
    }

    // The style's own button, labelled from the application palette for the reason the heading is (see `sys`).
    component KitDialogPlainButton: KitDialogButtonBase {
        id: plainButton
        palette.buttonText: plainButton.activeInk
    }

    // The style's own button, its label in the row's status role -- shaded where the hue would not read
    // (statusTextOn) on whatever fill the style shows now: at rest, hovered, pressed. Only the label is this
    // dialog's, so the fill stays the style's, and the ink is read from it without writing the palette the
    // fill is drawn from (Fusion's is): no loop, and no frame of a stale ink. Disabled, it is the palette's
    // disabled text, as any other disabled button's.
    component KitDialogRoleInWordsButton: KitDialogButtonBase {
        id: wordsButton
        /// The opaque colour the button is drawn on.
        required property color surface
        /// What the label is drawn on now: the style's fill, over the surface.
        readonly property color ground: Qt.tint(wordsButton.surface,
                                                wordsButton.background && wordsButton.background.color !== undefined
                                                ? wordsButton.background.color : "transparent")
        readonly property color ink: !wordsButton.enabled ? wordsButton.disabledInk
                                   : chromeStyle.paletteRevision < 0 ? "transparent"
                                   : chromeStyle.statusTextOn(wordsButton.row.statusRole, wordsButton.ground)
        contentItem: Label {
            text: wordsButton.text
            font: wordsButton.font
            color: wordsButton.ink
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    // Where a row's button stands: it loads the button of the row's look, and holds the focus the default takes
    // (a Loader is a focus scope).
    component KitDialogButtonSlot: Loader {
        id: slot
        /// One of the dialog's _buttonRows.
        required property var row
        /// A KitDialog.ButtonLook.
        required property int look
        /// The StatusColorRole it is filled with, as the default.
        required property int fillRole
        /// The opaque colour the dialog's buttons are drawn on.
        required property color surface
        visible: slot.row.shown
        focus: slot.look === KitDialog.Filled
        sourceComponent: slot.look === KitDialog.Filled ? filledComponent
                       : slot.look === KitDialog.RoleInWords ? roleInWordsComponent
                       : plainComponent

        Component {
            id: filledComponent
            KitDialogFilledButton { row: slot.row; fillRole: slot.fillRole }
        }
        Component {
            id: roleInWordsComponent
            KitDialogRoleInWordsButton { row: slot.row; surface: slot.surface }
        }
        Component {
            id: plainComponent
            KitDialogPlainButton { row: slot.row }
        }
    }

    // The default button, filled in its status colour.
    component KitDialogFilledButton: KitDialogButtonBase {
        id: filledButton
        /// The StatusColorRole it is filled with.
        required property int fillRole
        // Reads paletteRevision so the bindings re-evaluate on a light/dark flip (see UiStyleProvider).
        readonly property color fill: chromeStyle.paletteRevision < 0
                                      ? "transparent" : chromeStyle.statusColor(filledButton.fillRole)
        // Not "onFill": QML reads a name of "on" and a capital as a signal handler, and the property
        // never held a colour -- the label fell back to black, under 4.5:1 on the danger fill.
        readonly property color inkOnFill: chromeStyle.paletteRevision < 0
                                        ? "transparent" : chromeStyle.onStatusColor(filledButton.fillRole)

        contentItem: Label {
            text: filledButton.text
            font: filledButton.font
            color: filledButton.inkOnFill
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            implicitWidth: filledButton.contentItem.implicitWidth + chromeStyle.space.s8 + chromeStyle.space.s8
            implicitHeight: chromeStyle.size.control
            radius: chromeStyle.radii.control
            color: filledButton.fill
        }
    }

    // A Popup is not an Item and cannot carry the Accessible or Keys attached properties, so the content does.
    contentItem: ColumnLayout {
        spacing: chromeStyle.space.s4

        Accessible.role: Accessible.Dialog
        Accessible.name: root.heading
        Accessible.description: root.description

        // Enter takes the default button -- the press of a key that was struck, not the repeats of one still
        // held down since before the dialog appeared (the Return that chose a row of the switcher, which
        // opened this dialog a moment later, must not answer it).
        Keys.onReturnPressed: (event) => { if (!event.isAutoRepeat) root.pressDefault() }
        Keys.onEnterPressed: (event) => { if (!event.isAutoRepeat) root.pressDefault() }
        // Reached only where the popup keeps Escape for itself (no CloseOnEscape).
        Keys.onEscapePressed: root.escaped()

        Label {
            objectName: "kitDialogHeading"
            Layout.fillWidth: true
            text: root.heading
            font: chromeStyle.type.bodyStrong
            color: sys.windowText
            wrapMode: Text.Wrap
            // A heading can name what a program chose (a title, an extension's name): text, never markup.
            textFormat: Text.PlainText
        }

        // Taking no room without a message: the body then starts where the message would.
        Label {
            objectName: "kitDialogMessage"
            Layout.fillWidth: true
            visible: root.message.length > 0
            text: root.message
            font: chromeStyle.type.footnote
            color: sys.windowText
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }

        ColumnLayout {
            id: bodySlot
            objectName: "kitDialogBody"
            Layout.fillWidth: true
            spacing: chromeStyle.space.s3
            visible: children.length > 0
        }

        // The default button -- the one Enter takes and the focus starts on -- is the filled one, as a native
        // dialog draws it (macOS, GNOME); the other is the style's own. A Button's delegates are the style's or
        // its own, never one then the other, so each row's slot loads the button of its look (_lookOf), named
        // after the row's role whichever look it has. The slots stand, and take Tab, in the rows' order.
        RowLayout {
            id: buttonRow
            Layout.fillWidth: true
            Layout.topMargin: chromeStyle.space.s4
            spacing: chromeStyle.space.s4

            Item { Layout.fillWidth: true }

            // One slot per row, in the rows' order, declared rather than repeated: what a Repeater makes is no
            // child of the dialog's object tree, and callers find a button by its name there.
            KitDialogButtonSlot {
                row: root._buttonRows[0]
                look: root._lookOf(root._buttonRows[0])
                fillRole: root._fillRoleOf(root._buttonRows[0])
                surface: root.background ? root.background.color : sys.window
            }
            KitDialogButtonSlot {
                row: root._buttonRows[1]
                look: root._lookOf(root._buttonRows[1])
                fillRole: root._fillRoleOf(root._buttonRows[1])
                surface: root.background ? root.background.color : sys.window
            }
        }
    }
}
