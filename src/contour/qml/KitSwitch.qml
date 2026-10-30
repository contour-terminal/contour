// vim:syntax=qml
// The kit's switch: a track with a knob where the style draws shapes, the style's checkbox glyph
// (`switchOnGlyph` / `switchOffGlyph`) where it draws cells.
//
// It holds no state of its own: `checked` is the caller's, and a press or Space or Return asks for the
// opposite through `toggled`. A caller that accepts it assigns `checked` (usually through the model the
// value comes back from), so a switch can never show a value nobody stored.
//
// Not editable, it is disabled: dimmed to `settings.disabledOpacity`, and it takes no input.
import QtQuick
import QtQuick.Controls

Control {
    id: root
    objectName: "kitSwitch"

    /// Whether the switch is on.
    property bool checked: false
    /// What a screen reader calls the switch.
    property string name
    /// What a screen reader reads after the name.
    property string description
    /// Whether the user can change it.
    property bool editable: true

    /// The user asked for @p checked.
    signal toggled(bool checked)

    /// Asks for the opposite state.
    function toggle() {
        root.toggled(!root.checked)
    }

    // Cells where the style names a glyph for the switch, shapes where it does not.
    readonly property bool _drawsGlyph: chromeStyle.switchOnGlyph.length > 0
    // The track and the knob are pills: their radius is this share of their height.
    readonly property real _pill: 0.5
    // The knob is inset from the track on both sides.
    readonly property int _insetSides: 2

    enabled: root.editable
    opacity: root.enabled ? 1 : chromeStyle.settings.disabledOpacity
    activeFocusOnTab: true
    implicitWidth: chromeStyle.settings.switchWidth
    implicitHeight: chromeStyle.settings.switchHeight

    Accessible.role: Accessible.CheckBox
    Accessible.name: root.name
    Accessible.description: root.description
    Accessible.checkable: true
    Accessible.checked: root.checked
    Accessible.focusable: true
    Accessible.onPressAction: root.toggle()
    Accessible.onToggleAction: root.toggle()

    Keys.onSpacePressed: root.toggle()
    Keys.onReturnPressed: root.toggle()

    TapHandler {
        onTapped: {
            root.forceActiveFocus();
            root.toggle();
        }
    }

    // The shape row: a track, a knob that travels from one end to the other, and the focus ring.
    Item {
        anchors.fill: parent
        visible: !root._drawsGlyph

        Rectangle {
            objectName: "switchTrack"
            anchors.fill: parent
            radius: height * root._pill
            color: root.checked ? chromeStyle.colors.switchOnFill : chromeStyle.colors.switchOffFill
        }

        Rectangle {
            objectName: "switchKnob"
            readonly property real _inset: chromeStyle.space.s1
            width: parent.height - root._insetSides * _inset
            height: width
            radius: width * root._pill
            y: _inset
            x: root.checked ? parent.width - width - _inset : _inset
            color: chromeStyle.colors.switchKnob

            // Under Reduce Motion (CrossFade) the knob jumps.
            Behavior on x {
                NumberAnimation {
                    objectName: "switchKnobAnimation"
                    duration: chromeStyle.motion.animates ? chromeStyle.motion.fast : 0
                    easing.type: Easing.InOutQuad
                }
            }
        }

        Rectangle {
            objectName: "switchFocusRing"
            anchors.fill: parent
            anchors.margins: -chromeStyle.borderWidth
            radius: height * root._pill
            color: "transparent"
            border.width: chromeStyle.borderWidth
            border.color: chromeStyle.colors.settingsSelectionFill
            visible: root.visualFocus
        }
    }

    // The cell row: the checkbox glyph, reversed while it has the focus.
    Rectangle {
        anchors.fill: parent
        visible: root._drawsGlyph
        color: root.visualFocus ? chromeStyle.colors.settingsSelectionFill : "transparent"

        Label {
            objectName: "switchGlyph"
            anchors.centerIn: parent
            text: root.checked ? chromeStyle.switchOnGlyph : chromeStyle.switchOffGlyph
            font: chromeStyle.type.body
            color: root.visualFocus ? chromeStyle.colors.settingsSelectionText
                                    : (root.checked ? chromeStyle.colors.switchOnFill
                                                    : chromeStyle.colors.switchOffFill)
            Accessible.ignored: true
        }
    }
}
