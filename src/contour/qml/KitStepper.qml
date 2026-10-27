// vim:syntax=qml
// The kit's stepper: a number field with a pair of arrows, in the arrangement the style's
// `settingsTraits.stepper` names -- stacked beside the field, side by side, or inline at its trailing edge.
// The Terminal row draws the style's `stepperGlyph` pair instead.
//
// `value` is the caller's. The arrows, Up and Down, and a typed number committed with Return or by leaving
// the field, ask for a new one through `committed`; a number outside `from`..`to` is held to it, and one
// that is no number puts the old value back. Nothing is emitted for a value equal to the current one, so
// stepping against `to` is silent.
//
// Not editable, it is disabled: dimmed to `settings.disabledOpacity`, and it takes no input.
import QtQuick
import QtQuick.Controls

FocusScope {
    id: root
    objectName: "kitStepper"

    /// The current value.
    property real value: 0
    /// The least value.
    property real from: 0
    /// The greatest value.
    property real to: 100
    /// How far an arrow moves the value.
    property real stepSize: 1
    /// How many decimals the field shows and the value is rounded to.
    property int decimals: 0
    /// The unit shown after the field; empty shows none.
    property string unitText
    /// What a screen reader calls the stepper.
    property string name
    /// What a screen reader reads after the name.
    property string description
    /// Whether the user can change it.
    property bool editable: true

    /// The user asked for @p value.
    signal committed(real value)

    // Cells where the style names a glyph for the arrows, shapes where it does not.
    readonly property bool _drawsGlyph: chromeStyle.stepperGlyph.length > 0
    // The arrows' directions, up first; every arrangement draws one arrow for each.
    readonly property var _directions: [1, -1]
    // An arrow's label has padding on both sides.
    readonly property int _paddedSides: 2
    readonly property int _arrangement: chromeStyle.settingsTraits.stepper
    readonly property bool _stacked: root._arrangement === chromeStyle.settingsTraits.stacked
    readonly property bool _inline: root._arrangement === chromeStyle.settingsTraits.inlineArrows

    /// @return @p number held to `from`..`to` and rounded to `decimals`.
    function clamped(number) {
        const scale = Math.pow(10, root.decimals);
        return Math.min(root.to, Math.max(root.from, Math.round(number * scale) / scale));
    }

    /// Asks for @p number, held to the range, unless it is the current value.
    function commit(number) {
        const next = root.clamped(number);
        if (next !== root.value)
            root.committed(next);
    }

    /// Asks for the value @p count steps away.
    function stepBy(count) {
        root.commit(root.value + count * root.stepSize);
    }

    /// Shows the value in the field again, dropping what was typed.
    function showValue() {
        field.text = Qt.binding(() => root.value.toFixed(root.decimals));
    }

    enabled: root.editable
    opacity: root.enabled ? 1 : chromeStyle.settings.disabledOpacity
    implicitWidth: row.implicitWidth
    implicitHeight: chromeStyle.settings.control

    Accessible.role: Accessible.SpinBox
    Accessible.name: root.name
    Accessible.description: root.description
    Accessible.focusable: true
    Accessible.onIncreaseAction: root.stepBy(1)
    Accessible.onDecreaseAction: root.stepBy(-1)

    Row {
        id: row
        height: parent.height
        spacing: chromeStyle.space.s3

        Rectangle {
            id: fieldBox
            objectName: "stepperFieldBox"
            width: chromeStyle.settings.stepperField
            height: parent.height
            radius: chromeStyle.radii.control
            color: chromeStyle.colors.controlFill

            TextField {
                id: field
                objectName: "stepperField"
                focus: true
                anchors.fill: parent
                anchors.rightMargin: root._inline ? inlineArrows.width : 0
                background: null
                horizontalAlignment: TextInput.AlignRight
                font: chromeStyle.type.body
                color: chromeStyle.colors.controlText
                selectByMouse: true
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                text: root.value.toFixed(root.decimals)
                Accessible.ignored: true

                Keys.onUpPressed: root.stepBy(1)
                Keys.onDownPressed: root.stepBy(-1)
                Keys.onEscapePressed: {
                    root.showValue();
                    field.focus = false;
                }
                // Return, or leaving the field.
                onEditingFinished: {
                    const typed = Number(field.text.replace(",", "."));
                    if (field.text.trim().length > 0 && Number.isFinite(typed))
                        root.commit(typed);
                    root.showValue();
                }
            }

            // The arrows at the field's trailing edge.
            Row {
                id: inlineArrows
                anchors.right: parent.right
                height: parent.height
                visible: root._inline && !root._drawsGlyph
                width: visible ? implicitWidth : 0
                Repeater {
                    model: root._inline && !root._drawsGlyph ? root._directions : []
                    delegate: KitStepperArrow {
                        required property int modelData
                        direction: modelData
                        height: fieldBox.height
                    }
                }
            }
        }

        Label {
            objectName: "stepperUnit"
            anchors.verticalCenter: parent.verticalCenter
            visible: root.unitText.length > 0
            text: root.unitText
            font: chromeStyle.type.body
            color: chromeStyle.colors.settingsHelp
            Accessible.ignored: true
        }

        // Beside the field: stacked, or side by side.
        Loader {
            objectName: "stepperArrows"
            anchors.verticalCenter: parent.verticalCenter
            active: !root._inline && !root._drawsGlyph
            sourceComponent: root._stacked ? stackedArrows : sideBySideArrows
        }

        // The Terminal row's pair, one cell each.
        Row {
            objectName: "stepperGlyphs"
            visible: root._drawsGlyph
            Repeater {
                model: root._drawsGlyph ? root._directions : []
                delegate: KitStepperArrow {
                    required property int modelData
                    direction: modelData
                    height: row.height
                }
            }
        }
    }

    Component {
        id: stackedArrows
        Column {
            Repeater {
                model: root._directions
                delegate: KitStepperArrow {
                    required property int modelData
                    direction: modelData
                    height: fieldBox.height / root._directions.length
                }
            }
        }
    }

    Component {
        id: sideBySideArrows
        Row {
            spacing: chromeStyle.space.s2
            Repeater {
                model: root._directions.slice().reverse()
                delegate: KitStepperArrow {
                    required property int modelData
                    direction: modelData
                    height: fieldBox.height
                }
            }
        }
    }

    // One arrow: a press steps once.
    component KitStepperArrow: Rectangle {
        id: arrow
        /// +1 for the up arrow, -1 for the down arrow.
        property int direction: 1
        objectName: arrow.direction > 0 ? "stepperUp" : "stepperDown"
        implicitWidth: arrowLabel.implicitWidth + root._paddedSides * chromeStyle.space.s3
        radius: chromeStyle.radii.control
        color: "transparent"

        Label {
            id: arrowLabel
            anchors.centerIn: parent
            // The glyph pair is "up down", one character each; the shapes use triangles, or a minus and a
            // plus where the arrows sit side by side.
            text: root._drawsGlyph ? chromeStyle.stepperGlyph.charAt(arrow.direction > 0 ? 0 : 1)
                  : (!root._stacked && !root._inline) ? (arrow.direction > 0 ? "+" : "−")
                  : (arrow.direction > 0 ? "▴" : "▾")
            font: chromeStyle.type.body
            // On the field's fill where inline, on the group beside it.
            color: root._inline ? chromeStyle.colors.controlText : chromeStyle.colors.settingsLabel
            Accessible.ignored: true
        }

        TapHandler {
            onTapped: root.stepBy(arrow.direction)
        }
    }
}
