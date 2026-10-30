// vim:syntax=qml
// The kit's slider: a track with a knob and the formatted value trailing it ("15 %"). The Terminal row draws
// it in cells -- the style's fill, knob and track glyphs over `settings.slider` of them.
//
// `value` is the caller's. A key step (Left and Down lower it, Right and Up raise it) asks for the new value
// at once through `committed`; a drag asks once, on release, with the value the knob was let go at. Nothing
// is emitted for a value equal to the current one.
//
// Not editable, it is disabled: dimmed to `settings.disabledOpacity`, and it takes no input.
import QtQuick
import QtQuick.Controls

Control {
    id: root
    objectName: "kitSlider"

    /// The current value.
    property real value: 0
    /// The least value.
    property real from: 0
    /// The greatest value.
    property real to: 1
    /// How far a key moves the value.
    property real stepSize: 0.01
    /// How many decimals the shown number has.
    property int decimals: 0
    /// What the shown number is the value times: 100 shows a fraction as a percentage.
    property real displayFactor: 1
    /// The unit shown after the number; empty shows none.
    property string unitText
    /// What a screen reader calls the slider.
    property string name
    /// What a screen reader reads after the name.
    property string description
    /// Whether the user can change it.
    property bool editable: true

    /// The user asked for @p value.
    signal committed(real value)

    // Cells where the style names glyphs for the slider, shapes where it does not.
    readonly property bool _drawsGlyph: chromeStyle.sliderKnobGlyph.length > 0
    // The rail, its lit part and the knob are pills: their radius is this share of their thickness.
    readonly property real _pill: 0.5
    // The knob's value while a drag is under way; the caller's value otherwise.
    property real _shown: root.value
    readonly property real _fraction: root.to > root.from ? (root._shown - root.from) / (root.to - root.from) : 0
    // The track's and the knob's thickness, from the spacing scale and the switch.
    readonly property real _trackThickness: chromeStyle.space.s2
    readonly property real _knobSize: chromeStyle.settings.switchHeight

    /// @return @p number held to `from`..`to` and rounded to a step.
    function clamped(number) {
        const steps = Math.round((number - root.from) / root.stepSize);
        return Math.min(root.to, Math.max(root.from, root.from + steps * root.stepSize));
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

    /// @return The value the track position @p x stands for.
    function valueAt(x) {
        return root.from + Math.min(1, Math.max(0, x / track.width)) * (root.to - root.from);
    }

    /// The formatted value: the number, then the unit.
    readonly property string valueText: (root._shown * root.displayFactor).toFixed(root.decimals)
                                        + (root.unitText.length > 0 ? " " + root.unitText : "")

    // A new value from the caller ends whatever the knob showed.
    onValueChanged: root._shown = root.value

    enabled: root.editable
    opacity: root.enabled ? 1 : chromeStyle.settings.disabledOpacity
    activeFocusOnTab: true
    implicitWidth: chromeStyle.settings.slider + chromeStyle.space.s5 + valueLabel.implicitWidth
    implicitHeight: chromeStyle.settings.control

    Accessible.role: Accessible.Slider
    Accessible.name: root.name
    Accessible.description: root.description
    Accessible.focusable: true
    Accessible.onIncreaseAction: root.stepBy(1)
    Accessible.onDecreaseAction: root.stepBy(-1)

    Keys.onLeftPressed: root.stepBy(-1)
    Keys.onDownPressed: root.stepBy(-1)
    Keys.onRightPressed: root.stepBy(1)
    Keys.onUpPressed: root.stepBy(1)

    contentItem: Row {
        spacing: chromeStyle.space.s5

        Item {
            id: track
            objectName: "sliderTrack"
            width: chromeStyle.settings.slider
            height: root.height

            // The cell row: the fill up to the knob, the knob, then the rest of the track.
            Label {
                objectName: "sliderGlyphs"
                visible: root._drawsGlyph
                anchors.verticalCenter: parent.verticalCenter
                readonly property int _cells: Math.round(chromeStyle.settings.slider / chromeStyle.cellWidth)
                readonly property int _knobCell: Math.round(root._fraction * (_cells - 1))
                text: root._drawsGlyph
                      ? chromeStyle.sliderFillGlyph.repeat(_knobCell) + chromeStyle.sliderKnobGlyph
                        + chromeStyle.sliderTrackGlyph.repeat(_cells - 1 - _knobCell)
                      : ""
                font: chromeStyle.type.body
                color: root.visualFocus ? chromeStyle.colors.settingsLabel : chromeStyle.colors.switchOnFill
                Accessible.ignored: true
            }

            // The shape row: the whole track, the lit part of it, and the knob.
            Item {
                anchors.fill: parent
                visible: !root._drawsGlyph

                Rectangle {
                    objectName: "sliderRail"
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: root._trackThickness
                    radius: height * root._pill
                    color: chromeStyle.colors.switchOffFill
                }

                Rectangle {
                    objectName: "sliderFill"
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * root._fraction
                    height: root._trackThickness
                    radius: height * root._pill
                    color: chromeStyle.colors.switchOnFill
                }

                Rectangle {
                    objectName: "sliderKnob"
                    anchors.verticalCenter: parent.verticalCenter
                    x: (parent.width - width) * root._fraction
                    width: root._knobSize
                    height: root._knobSize
                    radius: width * root._pill
                    color: chromeStyle.colors.switchKnob
                    border.width: root.visualFocus ? chromeStyle.borderWidth : 0
                    border.color: chromeStyle.colors.settingsSelectionFill
                }
            }

            // A press or a drag moves the knob; the release asks for where it stopped.
            MouseArea {
                anchors.fill: parent
                onPressed: mouse => {
                    root.forceActiveFocus();
                    root._shown = root.clamped(root.valueAt(mouse.x));
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        root._shown = root.clamped(root.valueAt(mouse.x));
                }
                onReleased: {
                    const stopped = root._shown;
                    root._shown = root.value;
                    root.commit(stopped);
                }
            }
        }

        Label {
            id: valueLabel
            objectName: "sliderValue"
            anchors.verticalCenter: parent.verticalCenter
            text: root.valueText
            font: chromeStyle.type.body
            color: chromeStyle.colors.settingsHelp
            Accessible.ignored: true
        }
    }
}
