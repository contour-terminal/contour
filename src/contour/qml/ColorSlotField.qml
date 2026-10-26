// vim:syntax=qml
// A colour that may be unset: a "#RRGGBB" field beside a swatch, where clearing the field unsets the
// colour and a diagonal stroke through the swatch marks it as unset. Where the caller offers roles
// (`roleNames`, e.g. "accent"), the field takes a role's name as well, and keeps it as the name: a role
// is resolved against the scheme when drawn, so the swatch shows no fill for it rather than a guess.
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
    /// The colour as "#rrggbb" or a role's name; meaningful only while isSet.
    property string value: ""
    property bool editable: true
    /// The role names the field accepts beside a hex colour; none by default.
    property var roleNames: []
    /// The objectName of the text field, so a form holding several of these can tell them apart.
    property string fieldName: "colorSlotField"

    /// Whether @p text is a colour the field reports: a complete hex colour, or one of roleNames.
    function isColor(text) {
        return /^#[0-9a-fA-F]{6}$/.test(text) || root.roleNames.indexOf(text) >= 0
    }

    /// Emitted when the user sets or clears the colour; @p hex is the hex colour or the role's name.
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
        objectName: root.fieldName
        Layout.preferredWidth: 130
        enabled: root.editable
        text: root.isSet ? root.value : ""
        placeholderText: root.roleNames.length > 0 ? qsTr("#RRGGBB or role") : qsTr("#RRGGBB")
        selectByMouse: true
        // Reported only once the text is empty or a complete colour, so half-typed input neither sets a
        // nonsense colour nor clears the one already there.
        onTextChanged: {
            var trimmed = text.trim()
            if (trimmed.length === 0)
                root.changed(false, "")
            else if (root.isColor(trimmed))
                root.changed(true, trimmed)
        }
    }

    Rectangle {
        Layout.preferredWidth: 26
        Layout.preferredHeight: 26
        radius: 5
        // Filled only for a hex colour: a role's name is no QML colour.
        color: root.isSet && root.value.startsWith("#") ? root.value : "transparent"
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
