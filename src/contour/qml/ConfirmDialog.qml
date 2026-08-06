// vim:syntax=qml
// A small modal confirmation dialog (Cancel / destructive-confirm), theme-aware. Used before
// irreversible actions such as deleting a profile or color scheme.
//
// Built on QtQuick.Controls' in-scene Dialog — NOT QtQuick.Dialogs, which is not present on every target
// and whose hard import would break page loading where it is absent (same reason ColorSchemeEditor uses
// a hex field rather than a native colour dialog).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root


    modal: true
    anchors.centerIn: Overlay.overlay
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    /// The bold heading line.
    property string heading: qsTr("Confirm")
    /// The explanatory body text.
    property string message: ""
    /// The label on the destructive (accept) button.
    property string confirmText: qsTr("Delete")
    /// Emitted when the user confirms; the dialog closes itself afterwards.
    signal confirmed()

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }

    background: Rectangle {
        color: sys.window
        border.color: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.22)
        border.width: 1
        radius: 8
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            text: root.heading
            font.pointSize: 14
            font.weight: Font.DemiBold
            color: sys.windowText
            Layout.fillWidth: true
            Layout.topMargin: 18
            Layout.leftMargin: 18
            Layout.rightMargin: 18
        }
        Label {
            text: root.message
            wrapMode: Text.WordWrap
            color: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.78)
            Layout.fillWidth: true
            Layout.maximumWidth: 360
            Layout.leftMargin: 18
            Layout.rightMargin: 18
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 18
            spacing: 8
            Item { Layout.fillWidth: true }
            Button {
                objectName: "confirmCancelButton"
                text: qsTr("Cancel")
                onClicked: root.close()
            }
            Button {
                objectName: "confirmAcceptButton"
                text: root.confirmText
                onClicked: {
                    root.confirmed();
                    root.close();
                }
                contentItem: Label {
                    text: root.confirmText
                    color: "#ffffff"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 4
                    implicitWidth: 84
                    implicitHeight: 30
                    color: parent.down ? "#a01f1f" : (parent.hovered ? "#d23b3b" : "#c62828")
                }
            }
        }
    }
}
