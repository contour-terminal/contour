// vim:syntax=qml
// The kit's tooltip: a label with its keyboard shortcut dimmed beside it, in a box that each chrome row
// shapes by its tokens (chromeStyle.colors.tooltip*, radii.tooltip, size.tooltip*, elevation.e2), so this
// file never learns which row is active.
//
// Declared inside the control it describes, which drives `wanted` (usually its `hovered`). The tooltip
// shows after chromeStyle.motion.tooltipDelay of that, or at once while a tooltip showed within
// motion.tooltipWarm (the window's KitToolTipClock, on its ThemedWindow); it opens below its control, at the
// pointer, clamped inside the window, and flips above only where there is no room below.
//
// The text is always plain: a tooltip often carries what a program wrote. Its first line is the label, in
// the primary colour; the lines after it are secondary.
import QtQuick
import QtQuick.Window
import QtQuick.Templates as T

T.ToolTip {
    id: root
    objectName: "kitToolTip"

    /// The keyboard shortcut, in the dimmed slot at the label's trailing side; empty leaves the slot out.
    property string shortcut
    /// The command whose bound chord is the shortcut, looked up in `shortcutSource` whenever the tooltip is
    /// about to show (a binding can change while the window lives); empty looks nothing up.
    property string shortcutCommand
    /// What answers `shortcutText(command)` with the chord bound to a command, or an empty string where none
    /// is: the window's controller.
    property var shortcutSource: null
    /// Whether the control wants its tooltip: the host's to bind, usually to its hover.
    property bool wanted: false
    /// The pointer's x in the control's coordinates, where the tooltip's leading edge goes; the control's
    /// centre where the host does not track the pointer.
    property real pointerX: parent ? parent.width / 2 : 0

    /// The gap between the control and the tooltip.
    readonly property real distance: chromeStyle.space.s2
    /// The distance the tooltip keeps from the window's edges.
    readonly property real windowMargin: chromeStyle.space.s2
    /// The pointer's offset from the tooltip's leading edge: the pointer lands inside the box.
    readonly property real pointerInset: chromeStyle.space.s3
    /// The label's lines, the first in the primary colour.
    readonly property var _lines: root.text.split("\n")
    readonly property string _first: root._lines[0]
    readonly property string _rest: root._lines.slice(1).join("\n")
    /// The window's clock when it has one; this tooltip's own otherwise (a window that is no ThemedWindow).
    readonly property var _clock: (parent && parent.Window.window && parent.Window.window.tooltipClock) || ownClock
    /// Whether the shortcut slot exists: there is a shortcut.
    readonly property bool shortcutShown: root.shortcut.length > 0
    /// Whether the secondary lines exist: the text has more than one line.
    readonly property bool secondaryShown: root._rest.length > 0

    KitToolTipClock {
        id: ownClock
    }

    padding: 0
    leftPadding: chromeStyle.size.tooltipPadX
    rightPadding: chromeStyle.size.tooltipPadX
    topPadding: chromeStyle.size.tooltipPadTop
    bottomPadding: chromeStyle.size.tooltipPadBottom
    margins: 0
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent
    // Never takes the pointer from what lies beneath it.
    focus: false
    modal: false

    onWantedChanged: root.sync()
    Component.onCompleted: root.sync()

    /// Shows the tooltip, after the delay, or hides it, to match `wanted`.
    function sync() {
        if (!root.wanted) {
            root.visible = false
            return
        }
        // Cold: the token's delay. Warm: none. Set before `visible`, which starts the delay.
        root.delay = root._clock.isWarm() ? 0 : chromeStyle.motion.tooltipDelay
        root.visible = true
    }

    onAboutToShow: {
        root.refreshShortcut()
        root.place()
    }
    // Placed again once open: the box has its size by then, which it may not have had as it began to show.
    onOpened: {
        root.place()
        root._clock.touch()
    }
    onClosed: root._clock.touch()

    /// Reads the shortcut of `shortcutCommand` from `shortcutSource`, where there are both.
    function refreshShortcut() {
        if (root.shortcutCommand.length > 0 && root.shortcutSource)
            root.shortcut = root.shortcutSource.shortcutText(root.shortcutCommand)
    }

    /// Puts the tooltip below its control at the pointer, inside the window; above only where below has no
    /// room.
    function place() {
        if (!parent)
            return
        const window = parent.Window.window
        const origin = parent.mapToItem(null, 0, 0)
        const limitX = window ? window.width : origin.x + width
        const limitY = window ? window.height : origin.y + parent.height + height
        const desired = origin.x + root.pointerX - root.pointerInset
        const left = Math.max(root.windowMargin, Math.min(desired, limitX - implicitWidth - root.windowMargin))
        x = left - origin.x
        const below = origin.y + parent.height + root.distance
        const fitsBelow = below + implicitHeight + root.windowMargin <= limitY
        y = fitsBelow ? parent.height + root.distance : -implicitHeight - root.distance
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: chromeStyle.motion.fast }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: chromeStyle.motion.fast }
    }

    contentItem: Item {
        id: content
        objectName: "kitToolTipContent"
        // The widest the text may grow before it wraps: the window, less the margins and the padding.
        readonly property real maxTextWidth: Math.max(160, (root.parent && root.parent.Window.window
                                                              ? root.parent.Window.window.width : 640)
                                                       - 2 * root.windowMargin - root.leftPadding - root.rightPadding)
        readonly property real shortcutBlock: root.shortcutShown ? chromeStyle.size.tooltipGap + Math.ceil(shortcutLabel.implicitWidth) : 0
        readonly property real textWidth: Math.min(Math.ceil(Math.max(firstLabel.implicitWidth, restLabel.implicitWidth)),
                                                   maxTextWidth - shortcutBlock)

        implicitWidth: textWidth + shortcutBlock
        implicitHeight: firstLabel.height + (root.secondaryShown ? restLabel.height : 0)

        Text {
            id: firstLabel
            objectName: "kitToolTipLabel"
            width: content.textWidth
            text: root._first
            // Never AutoText: a tooltip's text may be what an application wrote (a link's URI, a tab title,
            // a command line), and AutoText renders it as rich text whenever its first line looks like markup.
            textFormat: Text.PlainText
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.tooltipText
            wrapMode: Text.WrapAnywhere
        }

        Text {
            id: restLabel
            objectName: "kitToolTipSecondary"
            y: firstLabel.height
            width: content.textWidth
            visible: root.secondaryShown
            text: root._rest
            textFormat: Text.PlainText
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.tooltipTextSecondary
            wrapMode: Text.WrapAnywhere
        }

        Text {
            id: shortcutLabel
            objectName: "kitToolTipShortcut"
            anchors.right: parent.right
            y: 0
            visible: root.shortcutShown
            text: root.shortcut
            textFormat: Text.PlainText
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.tooltipTextSecondary
        }
    }

    background: Rectangle {
        objectName: "kitToolTipBackground"
        radius: chromeStyle.radii.tooltip
        color: chromeStyle.colors.tooltipFill
        border.width: chromeStyle.colors.tooltipEdge.a === 0 ? 0
                      : chromeStyle.size.tooltipEdge > 0 ? chromeStyle.size.tooltipEdge
                      : chromeStyle.hairline(Screen.devicePixelRatio)
        border.color: chromeStyle.colors.tooltipEdge

        ElevationShadow {
            anchors.fill: parent
            z: -1
            level: chromeStyle.elevation.e2
            radius: parent.radius
        }
    }
}
