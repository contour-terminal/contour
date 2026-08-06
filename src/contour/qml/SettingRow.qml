// vim:syntax=qml
// One editable setting: the label + help text on the left and a type-driven editor on the right, with a
// faint accent wash on hover.
//
// It is the reusable building block of the settings page (mirroring Windows Terminal's SettingContainer):
// a page is a column of these, each bound to one row of SettingsController.profileFields
// ({ key, label, help, type, value }). The `type` string selects the editor widget, so a new field
// type is a new case in the Loader below — not a change to every page.
//
// Two looks, chosen by `flat`. Standalone it draws its own rounded, subtly elevated card
// (SystemPalette.base over the page's window colour). Inside a SettingsSection it draws no card at all,
// because the section already is one: nesting a card per row inside a card per group reads as clutter
// and doubles every border the eye has to cross to find a field.
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

    /// Drop the card and draw a hover wash only; for rows inside a SettingsSection.
    property bool flat: false
    /// Draw a hairline above the row. Set on every row of a section but its first, to separate the rows
    /// the dropped card used to separate.
    property bool showSeparator: false

    // Emitted when the user commits a change; the page forwards it to SettingsController.setProfileField.
    signal edited(string key, var value)

    implicitHeight: rowLayout.implicitHeight + (root.flat ? 16 : 24)
    implicitWidth: rowLayout.implicitWidth + 32

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
    readonly property color subtleText: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.62)

    Rectangle {
        anchors.fill: parent
        radius: root.flat ? 6 : 8
        color: root.flat ? "transparent" : sys.base
        border.width: root.flat ? 0 : 1
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

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        height: 1
        visible: root.showSeparator
        color: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.08)
    }

    RowLayout {
        id: rowLayout
        anchors.fill: parent
        anchors.leftMargin: root.flat ? 10 : 16
        anchors.rightMargin: root.flat ? 10 : 16
        anchors.topMargin: root.flat ? 8 : 12
        anchors.bottomMargin: root.flat ? 8 : 12
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

    // Every editor names itself from the row's own label. The label is a SIBLING Label, so without
    // this a screen reader reaches an unlabelled control and can only say what KIND it is -- and that
    // is true of every field on the settings page at once. Set here rather than per call site, so a new
    // field type is still just a new case in the Loader above.
    Component {
        id: enumEditor
        ComboBox {
            Accessible.name: root.label
            Accessible.description: root.help
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
            Accessible.name: root.label
            Accessible.description: root.help
            checked: root.value === true
            enabled: root.editable
            onToggled: root.edited(root.fieldKey, checked)
        }
    }

    // The two numeric editors restore their displayed value instead of committing a non-number. A
    // validator rejects keystrokes that cannot *lead* to a valid number, but it does not make the field
    // non-empty: clear it and `editingFinished` still fires, parse still yields NaN, and QVariant turns
    // NaN into 0 — so clearing the field would silently persist zero. Restoring reads as "that was not a
    // number" and leaves the setting alone.
    Component {
        id: doubleEditor
        TextField {
            id: doubleField
            Accessible.name: root.label
            Accessible.description: root.help
            Layout.fillWidth: true
            text: root.value !== null && root.value !== undefined ? String(root.value) : ""
            enabled: root.editable
            selectByMouse: true
            validator: DoubleValidator {}
            onEditingFinished: {
                var parsed = parseFloat(doubleField.text)
                if (isNaN(parsed)) {
                    doubleField.text = root.value !== null && root.value !== undefined ? String(root.value) : ""
                    return
                }
                root.edited(root.fieldKey, parsed)
            }
        }
    }

    Component {
        id: intEditor
        TextField {
            id: intField
            Accessible.name: root.label
            Accessible.description: root.help
            Layout.fillWidth: true
            text: root.value !== null && root.value !== undefined ? String(root.value) : "0"
            enabled: root.editable
            selectByMouse: true
            // Full 32-bit signed range: the config fields this drives are plain ints (e.g.
            // read_buffer_size, which can legitimately exceed a megabyte, and history_max_lines, whose
            // -1 means unlimited), and a narrower cap would silently clamp such a value on display and
            // then persist the clamped number on the next edit.
            validator: IntValidator {
                bottom: -2147483647
                top: 2147483647
            }
            onEditingFinished: {
                var parsed = parseInt(intField.text, 10)
                if (isNaN(parsed)) {
                    intField.text = root.value !== null && root.value !== undefined ? String(root.value) : "0"
                    return
                }
                root.edited(root.fieldKey, parsed)
            }
        }
    }

    Component {
        id: stringEditor
        TextField {
            Accessible.name: root.label
            Accessible.description: root.help
            Layout.fillWidth: true
            text: root.value !== null && root.value !== undefined ? String(root.value) : ""
            enabled: root.editable
            selectByMouse: true
            onEditingFinished: root.edited(root.fieldKey, text)
        }
    }
}
