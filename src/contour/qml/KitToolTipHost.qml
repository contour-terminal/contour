// vim:syntax=qml
// What a control declares to have a tooltip: it covers the control, watches the pointer over it, and builds
// the KitToolTip only while one is wanted or still fading out, so a strip of tabs or a pane header's buttons
// carry none until the pointer reaches them.
//
// One show rule for every control: the pointer is over it, no button is down on it, and there is text. A
// control with a reason of its own to stay quiet (a tab being renamed) sets `suppressed`.
import QtQuick

Item {
    id: root
    objectName: "kitToolTipHost"

    /// The tooltip's text: its first line is the label, the lines after it are secondary. Empty: no tooltip.
    property string text
    /// A shortcut the tooltip names as it is, such as a key the control answers to whatever is configured.
    property string shortcut
    /// The command whose bound chord the tooltip names, asked of the window's controller when it is about to show.
    property string shortcutCommand
    /// Whether the control keeps its tooltip away for a reason of its own.
    property bool suppressed: false
    /// The tooltip's object name, for a test to find it by.
    property string toolTipName: "kitToolTip"

    /// Whether the control wants its tooltip now.
    readonly property bool wanted: hover.hovered && !press.active && !root.suppressed && root.text.length > 0
    /// The tooltip while there is one, else null.
    readonly property var toolTip: loader.item
    /// Whether the tooltip is showing, fade-out included: set by the tooltip, so the Loader keeping it alive
    /// reads no property of the object it loads.
    property bool open: false
    /// The pointer's x on the control: where it was until the tooltip showed, then kept.
    property real pointerX: width / 2

    anchors.fill: parent

    HoverHandler {
        id: hover
        onPointChanged: if (!root.open) root.pointerX = point.position.x
    }

    // Passive, so the control's own handlers still take the press: it only hides the tooltip.
    PointHandler {
        id: press
        acceptedButtons: Qt.AllButtons
    }

    Loader {
        id: loader
        // Kept while it fades out, so the exit transition plays.
        active: root.wanted || root.open
        sourceComponent: KitToolTip {
            parent: root
            objectName: root.toolTipName
            text: root.text
            shortcut: root.shortcut
            shortcutCommand: root.shortcutCommand
            pointerX: root.pointerX
            wanted: root.wanted
            onVisibleChanged: root.open = visible
        }
    }
}
