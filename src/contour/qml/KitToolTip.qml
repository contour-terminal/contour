// vim:syntax=qml
// The kit's tooltip: a label with its keyboard shortcut dimmed beside it, in a box that each chrome row
// shapes by its tokens (KitToolTipBackground, size.tooltip*), so this file never learns which row is active.
//
// Controls declare a KitToolTipHost, which builds this while one is wanted. It shows after
// chromeStyle.motion.tooltipDelay of `wanted`, or at once while a tooltip showed within motion.tooltipWarm
// (the KitToolTipClock of the control's window, if it has one); it opens below its control, at the pointer,
// and flips above only where there is no room below. The popup's margins keep it, and its shadow, inside the
// window.
//
// The text is always plain: a tooltip often carries what a program wrote. Its first line is the label, in
// the primary colour; the lines after it are secondary.
import QtQuick
import QtQuick.Templates as T

T.ToolTip {
    id: root
    objectName: "kitToolTip"

    /// The keyboard shortcut, in the dimmed slot at the label's trailing side; empty leaves the slot out.
    property string shortcut
    /// The command whose bound chord is the shortcut, asked of the window's controller (its `win`) whenever
    /// the tooltip is about to show (a binding can change while the window lives); empty looks nothing up.
    property string shortcutCommand
    /// Whether the control wants its tooltip: KitToolTipHost's to bind.
    property bool wanted: false
    /// The pointer's x in the control's coordinates, where the tooltip's leading edge goes; the control's
    /// centre where nothing tracks the pointer.
    property real pointerX: parent ? parent.width / 2 : 0

    /// The gap between the control and the tooltip.
    readonly property real distance: chromeStyle.space.s2
    /// The pointer's offset from the tooltip's leading edge: the pointer lands inside the box.
    readonly property real pointerInset: chromeStyle.space.s3
    /// The label's lines, the first in the primary colour.
    readonly property var _lines: root.text.split("\n")
    readonly property string _first: root._lines[0]
    readonly property string _rest: root._lines.slice(1).join("\n")
    /// The control's window, or null.
    readonly property var _window: parent ? parent.Window.window : null
    /// The window's clock; none where the window is no ThemedWindow, and then every tooltip is cold.
    readonly property var _clock: _window && _window.tooltipClock ? _window.tooltipClock : null
    /// What answers `shortcutText(command)`: the window's controller (Main.qml's `win`), if it has one.
    readonly property var _controller: _window && _window.win ? _window.win : null
    /// Whether the shortcut slot exists: there is a shortcut.
    readonly property bool shortcutShown: root.shortcut.length > 0
    /// Whether the secondary lines exist: the text has more than one line.
    readonly property bool secondaryShown: root._rest.length > 0

    // A template popup sizes itself by nothing: the box is its content and its padding.
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    leftPadding: chromeStyle.size.tooltipPadX
    rightPadding: chromeStyle.size.tooltipPadX
    topPadding: chromeStyle.size.tooltipPadTop
    bottomPadding: chromeStyle.size.tooltipPadBottom
    // The window keeps the box, and its shadow, this far from its edges.
    margins: background.shadowReach

    onWantedChanged: root.sync()
    Component.onCompleted: root.sync()

    /// Shows the tooltip, after the delay, or hides it, to match `wanted`.
    function sync() {
        if (!root.wanted) {
            root.visible = false
            return
        }
        // Cold: the token's delay. Warm: none. Set before `visible`, which starts the delay.
        root.delay = root._clock && root._clock.isWarm() ? 0 : chromeStyle.motion.tooltipDelay
        root.visible = true
    }

    onAboutToShow: {
        root.refreshShortcut()
        root.place()
    }
    // Placed again once open: the box has its size by then, which it may not have had as it began to show.
    onOpened: {
        root.place()
        root.touchClock()
    }
    onClosed: root.touchClock()

    /// Notes on the window's clock that a tooltip is showing, or has just hidden.
    function touchClock() {
        if (root._clock)
            root._clock.touch()
    }

    /// Reads the shortcut of `shortcutCommand` from the window's controller, where there are both.
    function refreshShortcut() {
        if (root.shortcutCommand.length > 0 && root._controller)
            root.shortcut = root._controller.shortcutText(root.shortcutCommand)
    }

    /// Puts the tooltip below its control at the pointer; above only where below has no room. The margins keep it
    /// inside the window sideways.
    function place() {
        if (!parent)
            return
        x = root.pointerX - root.pointerInset
        const bottom = parent.mapToItem(null, 0, parent.height).y + root.distance
        const fitsBelow = !_window || bottom + implicitHeight + root.margins <= _window.height
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
        // The widest the text may grow before it wraps: the token's, less the padding.
        readonly property real maxTextWidth: chromeStyle.size.tooltipMaxWidth - root.leftPadding - root.rightPadding
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

    background: KitToolTipBackground {}
}
