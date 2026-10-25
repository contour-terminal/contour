// vim:syntax=qml
// The kit's modal dialog: a heading, a message and an accept/reject pair on a PopupSurface.
//
// Built on QtQuick.Controls' in-scene Dialog, never QtQuick.Dialogs (see ConfirmDialog.qml). Where it
// appears is a token: a sheet attached to the window's top edge when `layout.dialog` is `layout.sheet`,
// centred otherwise. Its width, insets, fonts and colours all come from the `chromeStyle` tokens, so this
// file never learns which style is active.
//
// Keyboard: the modal popup keeps focus inside itself, Esc rejects it, and Enter takes the accept button.
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

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    width: chromeStyle.size.dialogWidth
    padding: chromeStyle.space.s8

    // The sheet hangs from the window's top edge; every other row centres.
    readonly property bool _isSheet: chromeStyle.layout.dialog === chromeStyle.layout.sheet
    x: parent ? (parent.width - width) / 2 : 0
    y: _isSheet || !parent ? 0 : (parent.height - height) / 2

    // The body type, bold: a copy, since the token's font is shared and a font cannot be both assigned and tweaked.
    readonly property font _headingFont: {
        let heading = chromeStyle.type.body;
        heading.bold = true;
        return heading;
    }

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

        // Enter takes the default (accept) button.
        Keys.onReturnPressed: root.accept()
        Keys.onEnterPressed: root.accept()

        Label {
            objectName: "kitDialogHeading"
            Layout.fillWidth: true
            text: root.heading
            font: root._headingFont
            color: sys.windowText
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "kitDialogMessage"
            Layout.fillWidth: true
            text: root.message
            font: chromeStyle.type.footnote
            color: sys.windowText
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: chromeStyle.space.s4
            spacing: chromeStyle.space.s4

            Item { Layout.fillWidth: true }

            Button {
                objectName: "kitDialogReject"
                // Labelled from the application palette for the reason the heading is (see `sys`).
                palette.buttonText: sys.buttonText
                text: root.rejectText
                font: chromeStyle.type.footnote
                Accessible.name: text
                onClicked: root.reject()
            }

            Button {
                id: acceptButton
                objectName: "kitDialogAccept"
                text: root.acceptText
                font: chromeStyle.type.footnote
                focus: true
                Accessible.name: text
                onClicked: root.accept()

                // Reads paletteRevision so the bindings re-evaluate on a light/dark flip (see UiStyleProvider).
                readonly property color fill: chromeStyle.paletteRevision < 0
                                              ? "transparent" : chromeStyle.statusColor(root.acceptRole)
                readonly property color onFill: chromeStyle.paletteRevision < 0
                                                ? "transparent" : chromeStyle.onStatusColor(root.acceptRole)

                contentItem: Label {
                    text: acceptButton.text
                    font: acceptButton.font
                    color: acceptButton.onFill
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
