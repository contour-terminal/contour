// vim:syntax=qml
// The color-scheme editor: a grid of color slots (default fg/bg + the 8 normal and 8 bright ANSI
// colors) bound to SettingsController.schemeColors ({ key, label, color }). Each slot shows a live
// swatch and an editable "#rrggbb" field; committing writes the slot back through
// SettingsController.setSchemeColor.
//
// A hex field is used deliberately instead of a native ColorDialog: QtQuick.Dialogs is not guaranteed
// present on every target, and a hard import of it would break page loading where it is absent.
//
// Dark/light is handled at the PROFILE level (a profile references two schemes); a single scheme is one
// palette, edited here, and either referenced directly or picked as the light or dark half of a profile.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    // The SettingsController (may be null during teardown; every access below is guarded).
    property var controller: null

    spacing: 12

    Label {
        text: (root.controller && root.controller.editingScheme.length > 0)
              ? qsTr("Color scheme: %1").arg(root.controller.editingScheme)
              : qsTr("New color scheme")
        font.pointSize: 13
        font.bold: true
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: 24
        rowSpacing: 8

        Repeater {
            model: root.controller ? root.controller.schemeColors : []
            delegate: RowLayout {
                required property var modelData
                spacing: 10
                Rectangle {
                    width: 26
                    height: 26
                    radius: 4
                    color: modelData.color
                    border.color: Qt.rgba(0, 0, 0, 0.35)
                    border.width: 1
                }
                Label { text: modelData.label; Layout.preferredWidth: 130 }
                TextField {
                    Layout.preferredWidth: 110
                    text: modelData.color
                    enabled: root.controller && !root.controller.locked
                    selectByMouse: true
                    font.family: "monospace"
                    inputMask: "\\#HHHHHH"
                    onEditingFinished: if (root.controller) root.controller.setSchemeColor(modelData.key, text)
                }
            }
        }
    }

    // Save / delete bar for the scheme.
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        TextField {
            id: schemeNameField
            objectName: "schemeNameField"
            Layout.preferredWidth: 200
            placeholderText: qsTr("Scheme name")
            text: root.controller ? root.controller.editingScheme : ""
            selectByMouse: true
        }
        Button {
            objectName: "saveSchemeButton"
            text: qsTr("Save scheme")
            enabled: root.controller && !root.controller.locked && schemeNameField.text.trim().length > 0
            onClicked: root.controller.saveColorScheme(schemeNameField.text.trim())
        }
        Button {
            objectName: "deleteSchemeButton"
            text: qsTr("Delete scheme")
            enabled: root.controller && !root.controller.locked
                     && root.controller.editingScheme.length > 0
            onClicked: root.controller.deleteColorScheme(root.controller.editingScheme)
        }
        Item { Layout.fillWidth: true }
    }
}
