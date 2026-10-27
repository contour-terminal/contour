// vim:syntax=qml
// The kit's colour well: a swatch of the colour beside a field for its hex, and the contrast readout
// beneath it where the caller supplies one. The Terminal row draws the swatch as the style's
// `colorWellGlyph`, in the colour.
//
// `hex` is the caller's. The field takes `#rgb` or `#rrggbb`; committed with Return or by leaving it, a
// valid entry asks for `#rrggbb` (lower case) through `committed`, and anything else puts the old hex back.
// A hex equal to the current one asks for nothing.
//
// Not editable, it is disabled: dimmed to `settings.disabledOpacity`, and it takes no input.
import QtQuick
import QtQuick.Controls

FocusScope {
    id: root
    objectName: "kitColorWell"

    /// The colour as "#rrggbb"; empty for none.
    property string hex
    /// The hex of the background the readout measures the colour against.
    property string against
    /// The contrast readout shown under the field, "{ratio} : 1"; empty shows none. Set by the caller,
    /// which knows the scheme the colour is judged in.
    property string readout
    /// What a screen reader calls the well.
    property string name
    /// What a screen reader reads after the name.
    property string description
    /// Whether the user can change it.
    property bool editable: true

    /// The user asked for the colour @p hex, as "#rrggbb".
    signal committed(string hex)

    // Cells where the style names a glyph for the well, a shape where it does not.
    readonly property bool _drawsGlyph: chromeStyle.colorWellGlyph.length > 0
    readonly property color _swatch: /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.test(root.hex) ? root.hex : "transparent"

    /// @return @p text as "#rrggbb", or an empty string where it is no colour.
    function normalized(text) {
        const match = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(text.trim());
        if (match === null)
            return "";
        const digits = match[1].length === 3 ? match[1].replace(/./g, digit => digit + digit) : match[1];
        return "#" + digits.toLowerCase();
    }

    /// Shows the hex in the field again, dropping what was typed.
    function showHex() {
        field.text = Qt.binding(() => root.hex);
    }

    enabled: root.editable
    opacity: root.enabled ? 1 : chromeStyle.settings.disabledOpacity
    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    Accessible.role: Accessible.EditableText
    Accessible.name: root.name
    Accessible.description: root.description
    Accessible.focusable: true

    Column {
        id: column
        spacing: chromeStyle.space.s2

        Row {
            spacing: chromeStyle.space.s3

            // The swatch.
            Item {
                width: chromeStyle.settings.colorWell
                height: chromeStyle.settings.colorWell
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    objectName: "colorWellSwatch"
                    anchors.fill: parent
                    visible: !root._drawsGlyph
                    radius: chromeStyle.radii.control
                    color: root._swatch
                    border.width: chromeStyle.borderWidth
                    border.color: chromeStyle.colors.settingsSeparator
                }

                Label {
                    objectName: "colorWellGlyph"
                    anchors.centerIn: parent
                    visible: root._drawsGlyph
                    text: chromeStyle.colorWellGlyph
                    font: chromeStyle.type.body
                    color: root._swatch
                    Accessible.ignored: true
                }
            }

            Rectangle {
                width: chromeStyle.settings.stepperField
                height: chromeStyle.settings.control
                radius: chromeStyle.radii.control
                color: chromeStyle.colors.controlFill

                TextField {
                    id: field
                    objectName: "colorHexField"
                    focus: true
                    anchors.fill: parent
                    background: null
                    font: chromeStyle.type.body
                    color: chromeStyle.colors.controlText
                    selectByMouse: true
                    text: root.hex
                    Accessible.name: root.name
                    Accessible.ignored: true

                    Keys.onEscapePressed: {
                        root.showHex();
                        field.focus = false;
                    }
                    // Return, or leaving the field.
                    onEditingFinished: {
                        const entered = root.normalized(field.text);
                        if (entered.length > 0 && entered !== root.hex.toLowerCase())
                            root.committed(entered);
                        root.showHex();
                    }
                }
            }
        }

        Label {
            objectName: "colorReadout"
            visible: root.readout.length > 0
            text: root.readout
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.settingsHelp
            Accessible.ignored: true
        }
    }
}
