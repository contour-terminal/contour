// vim:syntax=qml
// The kit's modal dialog: a heading, a message and an accept/reject pair on a PopupSurface.
//
// Built on QtQuick.Controls' in-scene Dialog, never QtQuick.Dialogs (see ConfirmDialog.qml). Where it
// appears is a token: a sheet attached to the window's top edge when `layout.dialog` is `layout.sheet`,
// centred otherwise. Its width, insets, fonts and colours all come from the `chromeStyle` tokens, so this
// file never learns which style is active.
//
// Keyboard: the modal popup keeps focus inside itself, Esc rejects it, and Enter takes the default button
// (the accept button, unless `defaultButton` names the reject one) -- never a disabled accept.
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

    /// Which button takes the initial focus and Enter.
    enum DefaultButton {
        Accept,
        Reject
    }
    /// A KitDialog.DefaultButton.
    property int defaultButton: KitDialog.Accept

    /// Escape was pressed while the dialog keeps it (its closePolicy has no CloseOnEscape): a dialog for
    /// which Escape means something else than its reject button answers it here.
    signal escaped()

    /// Takes the default button, as Enter does.
    function pressDefault() {
        if (root.defaultButton === KitDialog.Reject)
            root.reject();
        else if (root.acceptEnabled)
            root.accept();
    }

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    width: chromeStyle.size.dialogWidth
    padding: chromeStyle.space.s8

    // The sheet hangs from the window's top edge; every other row centres.
    readonly property bool _isSheet: chromeStyle.layout.dialog === chromeStyle.layout.sheet
    x: parent ? (parent.width - width) / 2 : 0
    y: _isSheet || !parent ? 0 : (parent.height - height) / 2

    // The text colours come from the application palette directly, as ConfirmDialog's do. The popup's
    // inherited palette is not enough: a binding to `root.palette.windowText` is taken before the popup
    // reaches its window and keeps the style's default (black) once the window's palette arrives, which on
    // a dark surface reads at under 2:1.
    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }

    background: PopupSurface {}

    // A Popup is not an Item and cannot carry the Accessible or Keys attached properties, so the content does.
    contentItem: ColumnLayout {
        spacing: chromeStyle.space.s4

        Accessible.role: Accessible.Dialog
        Accessible.name: root.heading
        Accessible.description: root.description

        // Enter takes the default button.
        Keys.onReturnPressed: root.pressDefault()
        Keys.onEnterPressed: root.pressDefault()
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

        Label {
            objectName: "kitDialogMessage"
            Layout.fillWidth: true
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

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: chromeStyle.space.s4
            spacing: chromeStyle.space.s4

            Item { Layout.fillWidth: true }

            Button {
                id: rejectButton
                objectName: "kitDialogReject"
                // A dialog with nothing to decline (About) names no reject text and shows one button.
                visible: root.rejectText.length > 0
                // Labelled from the application palette for the reason the heading is (see `sys`).
                palette.buttonText: sys.buttonText
                text: root.rejectText
                font: chromeStyle.type.footnote
                focus: root.defaultButton === KitDialog.Reject
                Accessible.name: text
                onClicked: root.reject()
            }

            Button {
                id: acceptButton
                objectName: "kitDialogAccept"
                text: root.acceptText
                font: chromeStyle.type.footnote
                enabled: root.acceptEnabled
                focus: root.defaultButton === KitDialog.Accept
                Accessible.name: text
                onClicked: root.accept()

                // Reads paletteRevision so the bindings re-evaluate on a light/dark flip (see UiStyleProvider).
                readonly property color fill: chromeStyle.paletteRevision < 0
                                              ? "transparent" : chromeStyle.statusColor(root.acceptRole)
                // Not "onFill": QML reads a name of "on" and a capital as a signal handler, and the property
                // never held a colour -- the label fell back to black, under 4.5:1 on the danger fill.
                readonly property color inkOnFill: chromeStyle.paletteRevision < 0
                                                ? "transparent" : chromeStyle.onStatusColor(root.acceptRole)

                contentItem: Label {
                    text: acceptButton.text
                    font: acceptButton.font
                    color: acceptButton.inkOnFill
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitWidth: acceptButton.contentItem.implicitWidth + chromeStyle.space.s8 + chromeStyle.space.s8
                    implicitHeight: chromeStyle.size.control
                    radius: chromeStyle.radii.control
                    color: acceptButton.fill
                }
            }
        }
    }
}
