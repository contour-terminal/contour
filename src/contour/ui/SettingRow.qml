// vim:syntax=qml
// One editable setting: a label + help text on the left, and a type-driven editor on the right.
//
// It is the reusable building block of the settings page (mirroring Windows Terminal's SettingContainer):
// a page is a column of these, each bound to one row of SettingsController.profileFields
// ({ key, label, help, type, value }). The `type` string selects the editor widget, so a new field
// type is a new case here — not a change to every page.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root

    property string fieldKey: ""
    property string label: ""
    property string help: ""
    property string type: "string"
    property var value: null
    property bool editable: true

    // Emitted when the user commits a change; the page forwards it to SettingsController.setProfileField.
    signal edited(string key, var value)

    spacing: 16

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2
        Label { text: root.label; font.bold: true }
        Label {
            text: root.help
            visible: root.help.length > 0
            opacity: 0.7
            font.pointSize: 9
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    Loader {
        id: editorLoader
        Layout.preferredWidth: 240
        sourceComponent: root.type === "bool" ? boolEditor
                       : root.type === "double" ? doubleEditor
                       : root.type === "int" ? intEditor
                       : stringEditor
    }

    Component {
        id: boolEditor
        Switch {
            checked: root.value === true
            enabled: root.editable
            onToggled: root.edited(root.fieldKey, checked)
        }
    }

    Component {
        id: doubleEditor
        TextField {
            Layout.fillWidth: true
            text: root.value !== null && root.value !== undefined ? String(root.value) : ""
            enabled: root.editable
            selectByMouse: true
            validator: DoubleValidator {}
            onEditingFinished: root.edited(root.fieldKey, parseFloat(text))
        }
    }

    Component {
        id: intEditor
        SpinBox {
            enabled: root.editable
            from: -1000000
            to: 1000000
            value: root.value !== null && root.value !== undefined ? root.value : 0
            onValueModified: root.edited(root.fieldKey, value)
        }
    }

    Component {
        id: stringEditor
        TextField {
            Layout.fillWidth: true
            text: root.value !== null && root.value !== undefined ? String(root.value) : ""
            enabled: root.editable
            selectByMouse: true
            onEditingFinished: root.edited(root.fieldKey, text)
        }
    }
}
