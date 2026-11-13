// vim:syntax=qml
// ScrollBar for the ContourTui style: a one-cell-wide track drawn with block characters, the way a TUI
// draws one. Kept always-visible rather than fading in, because a terminal scrollbar is part of the
// frame, not an overlay. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.ScrollBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 0
    // AsNeeded (the default) shows the bar only while there is something to scroll: a handle filling its
    // whole trough says nothing, and drew a bar at the side of every view whose content fits.
    // Short of the whole by more than rounding: a content that fits to within a float's error fits. The same
    // rule as ContourLinux/ScrollBar.qml's; separate styles, so stated in each.
    visible: control.policy === T.ScrollBar.AlwaysOn
             || (control.policy === T.ScrollBar.AsNeeded && control.size < 1.0 - 1e-6)

    // A handle of at least one whole cell, so it stays both visible and grabbable. QQuickScrollBar
    // defaults this to 0 and then sizes the handle at `size * trackLength` with no floor at all, so a
    // long enough list shrinks it to a sub-pixel sliver -- the trough is still drawn, but there is
    // nothing left to drag and the view can only be scrolled by wheel. One cell rather than the
    // built-in styles' square handle, because a cell is this style's smallest visible thing.
    // The zero guards are for the frame before the bar has a size; the quotient would be Infinity.
    minimumSize: control.orientation === Qt.Horizontal
                 ? (control.width > 0 ? chromeStyle.cellWidth / control.width : 0)
                 : (control.height > 0 ? chromeStyle.cellHeight / control.height : 0)

    contentItem: Rectangle {
        implicitWidth: chromeStyle.cellWidth
        implicitHeight: chromeStyle.cellHeight
        color: control.pressed ? control.palette.highlight : control.palette.mid
        opacity: control.enabled ? 1.0 : 0.5
    }

    background: Rectangle {
        implicitWidth: chromeStyle.cellWidth
        implicitHeight: chromeStyle.cellHeight
        // A visible trough: in a TUI the bar's track is drawn, not implied by empty space.
        color: chromeStyle.wash(control.palette.mid)
    }
}
