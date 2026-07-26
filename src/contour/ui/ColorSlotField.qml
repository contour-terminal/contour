// vim:syntax=qml
// A hex colour that may be unset: a "#RRGGBB" field beside a swatch, where clearing the field unsets the
// colour and a diagonal stroke through the swatch marks it as unset.
//
// Reports through a signal rather than writing anywhere, so the same widget serves each of the several
// optional colours an indicator item can carry.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root

    property string label: ""
    /// Whether the colour is set at all. An unset colour is not black -- it means "inherit".
    property bool isSet: false
    /// The colour as "#rrggbb"; meaningful only while isSet.
    property string value: ""
    property bool editable: true

    /// Emitted when the user sets or clears the colour.
    signal changed(bool isSet, string hex)

    // Live OS palette handle, so the field follows dark/light in realtime.
    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
    readonly property color subtleText: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.6)
    readonly property color hairline: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.12)

    spacing: 8

    Label {
        text: root.label
        color: sys.windowText
        Layout.preferredWidth: 130
    }

    TextField {
        objectName: "colorSlotField"
        Layout.preferredWidth: 130
        enabled: root.editable
        text: root.isSet ? root.value : ""
        placeholderText: qsTr("#RRGGBB")
        selectByMouse: true
        // Reported only once the text is empty or a complete colour, so half-typed input neither sets a
        // nonsense colour nor clears the one already there.
        onTextChanged: {
            var trimmed = text.trim()
            if (trimmed.length === 0)
                root.changed(false, "")
            else if (/^#[0-9a-fA-F]{6}$/.test(trimmed))
                root.changed(true, trimmed)
        }
    }

    Rectangle {
        Layout.preferredWidth: 26
        Layout.preferredHeight: 26
        radius: 5
        color: root.isSet ? root.value : "transparent"
        border.width: 1
        border.color: root.hairline

        // Marks "unset", so an unset slot is not mistaken for a black one.
        Rectangle {
            anchors.centerIn: parent
            visible: !root.isSet
            width: parent.width * 1.15
            height: 1
            rotation: -45
            color: root.subtleText
        }
    }

    Item { Layout.fillWidth: true }
}
