// vim:syntax=qml
// The kit's segmented control: one segment per option, the chosen one lit.
//
// `value` is the caller's; a press on a segment, or the arrow keys, ask for another through `picked`.
// Left and Right step to the neighbour and stop at the ends. The style's radio glyphs
// (`segmentOnGlyph` / `segmentOffGlyph`) lead each label where it draws cells.
//
// Not editable, it is disabled: dimmed to `settings.disabledOpacity`, and it takes no input.
import QtQuick
import QtQuick.Controls

Control {
    id: root
    objectName: "kitSegmented"

    /// The options' values, in order.
    property var options: []
    /// The options' labels, same length as `options`; empty shows the values.
    property var labels: []
    /// The chosen option's value.
    property string value
    /// What a screen reader calls the group.
    property string name
    /// What a screen reader reads after the name.
    property string description
    /// Whether the user can change it.
    property bool editable: true

    /// The user asked for the option @p value.
    signal picked(string value)

    // Cells where the style names a glyph for a segment, shapes where it does not.
    readonly property bool _drawsGlyph: chromeStyle.segmentOnGlyph.length > 0
    readonly property int _index: root.options.indexOf(root.value)
    // A segment's label has padding on both sides.
    readonly property int _paddedSides: 2

    /// Asks for the neighbour @p step places along; the ends stop it.
    function step(delta) {
        const next = root._index + delta;
        if (next >= 0 && next < root.options.length)
            root.picked(root.options[next]);
    }

    enabled: root.editable
    opacity: root.enabled ? 1 : chromeStyle.settings.disabledOpacity
    activeFocusOnTab: true
    implicitWidth: strip.implicitWidth
    implicitHeight: chromeStyle.settings.control

    Accessible.role: Accessible.Grouping
    Accessible.name: root.name
    Accessible.description: root.description
    Accessible.focusable: true

    Keys.onLeftPressed: root.step(-1)
    Keys.onRightPressed: root.step(1)

    // The control's own fill on every row: the Terminal row's labels are the control's text too. The
    // keyboard's place is a ring around the whole control on the shape rows; the Terminal row reverses
    // the chosen segment instead (see the segment).
    background: Rectangle {
        radius: chromeStyle.radii.control
        color: chromeStyle.colors.controlFill
        border.width: root.visualFocus && !root._drawsGlyph ? chromeStyle.borderWidth : 0
        border.color: chromeStyle.colors.settingsSelectionFill
    }

    contentItem: Row {
        id: strip
        spacing: root._drawsGlyph ? chromeStyle.cellWidth : 0

        Repeater {
            model: root.options

            delegate: Rectangle {
                id: segment
                required property int index
                required property var modelData
                readonly property bool isChosen: segment.modelData === root.value
                // The chosen segment is lit on the shape rows, and on the Terminal row while it has the focus.
                readonly property bool isLit: segment.isChosen && (!root._drawsGlyph || root.visualFocus)
                readonly property string text: root.labels.length > index ? root.labels[index] : segment.modelData
                objectName: "segment"

                implicitWidth: segmentLabel.implicitWidth + root._paddedSides * chromeStyle.space.s5
                height: root.height
                radius: chromeStyle.radii.control
                color: segment.isLit ? chromeStyle.colors.settingsSelectionFill : "transparent"

                Accessible.role: Accessible.RadioButton
                Accessible.name: segment.text
                Accessible.checkable: true
                Accessible.checked: segment.isChosen
                Accessible.onPressAction: root.picked(segment.modelData)

                Label {
                    id: segmentLabel
                    objectName: "segmentLabel"
                    anchors.centerIn: parent
                    text: root._drawsGlyph
                          ? (segment.isChosen ? chromeStyle.segmentOnGlyph : chromeStyle.segmentOffGlyph)
                            + " " + segment.text
                          : segment.text
                    font: chromeStyle.type.body
                    color: segment.isLit ? chromeStyle.colors.settingsSelectionText : chromeStyle.colors.controlText
                    Accessible.ignored: true
                }

                TapHandler {
                    onTapped: {
                        root.forceActiveFocus();
                        root.picked(segment.modelData);
                    }
                }
            }
        }
    }
}
