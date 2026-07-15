// vim:syntax=qml
// One editable setting, rendered as a Windows-Terminal-style "settings card": a rounded, subtly
// elevated surface (SystemPalette.base over the page's window colour) with the label + help text on
// the left and a type-driven editor on the right, plus a faint accent wash on hover.
//
// It is the reusable building block of the settings page (mirroring Windows Terminal's SettingContainer):
// a page is a column of these, each bound to one row of SettingsController.profileFields
// ({ key, label, help, type, value }). The `type` string selects the editor widget, so a new field
// type is a new case in the Loader below — not a change to every page.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string fieldKey: ""
    property string label: ""
    property string help: ""
    property string type: "string"
    property var value: null
    property var options: []
    property bool editable: true

    // Emitted when the user commits a change; the page forwards it to SettingsController.setProfileField.
    signal edited(string key, var value)

    implicitHeight: rowLayout.implicitHeight + 24
    implicitWidth: rowLayout.implicitWidth + 32

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
    readonly property color subtleText: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.62)

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: sys.base
        border.width: 1
        border.color: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.12)
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: sys.highlight
            opacity: cardHover.hovered ? 0.06 : 0.0
            Behavior on opacity { NumberAnimation { duration: 90 } }
        }
        HoverHandler { id: cardHover }
    }

    RowLayout {
        id: rowLayout
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: root.label
                font.weight: Font.DemiBold
                color: sys.windowText
            }
            Label {
                text: root.help
                visible: root.help.length > 0
                color: root.subtleText
                font.pointSize: 9
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Loader {
            id: editorLoader
            // A bool is a compact Switch hugged to the right; every other editor claims a fixed column.
            Layout.preferredWidth: root.type === "bool" ? -1 : 240
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            sourceComponent: root.type === "bool" ? boolEditor
                           : root.type === "double" ? doubleEditor
                           : root.type === "int" ? intEditor
                           : root.type === "enum" ? enumEditor
                           : stringEditor
        }
    }

    Component {
        id: enumEditor
        ComboBox {
            Layout.fillWidth: true
            enabled: root.editable
            model: root.options
            currentIndex: Math.max(0, root.options.indexOf(root.value))
            onActivated: root.edited(root.fieldKey, currentText)
        }
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
