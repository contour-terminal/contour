// vim:syntax=qml
// A status line item's colour, which may be unset: a field beside a swatch, where clearing the field unsets
// the colour and a diagonal stroke through the swatch marks it as unset. The field takes a "#RRGGBB" colour
// and, where the caller offers roles (`roleNames`, e.g. "accent"), a role's name, which it keeps as the name:
// a role is resolved against the scheme when drawn, so the swatch shows no fill for it rather than a guess.
//
// Reports through a signal rather than writing anywhere, so the same field serves each of the several
// optional colours an item can carry.
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

    /// The role @p text names, in any case as the template parser reads it, spelled as roleNames has it;
    /// "" when it names none.
    function roleNamed(text) {
        const wanted = text.toLowerCase();
        const found = root.roleNames.find(name => name.toLowerCase() === wanted);
        return found !== undefined ? found : "";
    }

    /// Emitted when the user sets or clears the colour; @p hex is the hex colour or the role's name.
    signal changed(bool isSet, string hex)

    spacing: chromeStyle.space.s5

    Label {
        Layout.preferredWidth: chromeStyle.settings.sidebarWidth
        text: root.label
        font: chromeStyle.type.body
        color: chromeStyle.colors.settingsLabel
        elide: Text.ElideRight
        textFormat: Text.PlainText
    }

    TextField {
        objectName: root.fieldName
        Layout.fillWidth: true
        enabled: root.editable
        text: root.isSet ? root.value : ""
        placeholderText: root.roleNames.length > 0 ? qsTr("#RRGGBB or role") : qsTr("#RRGGBB")
        font: chromeStyle.type.body
        selectByMouse: true
        Accessible.name: root.label
        // Reported only once the text is empty or a complete colour, so half-typed input neither sets a
        // nonsense colour nor clears the one already there.
        onTextChanged: {
            const trimmed = text.trim();
            if (trimmed.length === 0)
                root.changed(false, "");
            else if (/^#[0-9a-fA-F]{6}$/.test(trimmed))
                root.changed(true, trimmed);
            else if (root.roleNamed(trimmed).length > 0)
                root.changed(true, root.roleNamed(trimmed));
        }
    }

    Rectangle {
        Layout.preferredWidth: chromeStyle.settings.colorWell
        Layout.preferredHeight: chromeStyle.settings.colorWell
        radius: chromeStyle.radii.control
        // Filled only for a hex colour: a role's name is no QML colour.
        color: root.isSet && root.value.startsWith("#") ? root.value : "transparent"
        border.width: chromeStyle.borderWidth
        border.color: chromeStyle.colors.settingsLabel
        clip: true

        // Marks "unset", so an unset slot is not mistaken for a black one.
        Rectangle {
            anchors.centerIn: parent
            visible: !root.isSet
            width: parent.width * Math.SQRT2
            height: chromeStyle.borderWidth
            rotation: -45
            color: chromeStyle.colors.settingsHelp
        }
    }
}
