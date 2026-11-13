// vim:syntax=qml
// ScrollBar for the ContourLinux style: a slim rounded thumb in the kit's scrollbar inks over a faint
// track, as a GTK or Breeze overlay bar reads. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.ScrollBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: chromeStyle.space.s1
    // AsNeeded (the default) shows the bar only while there is something to scroll: a thumb as long as its
    // track says nothing, and drew a dark bar at the side of every view whose content fits -- a fingerprint
    // on one line, a short list.
    // Short of the whole by more than rounding: a content that fits to within a float's error fits. The same
    // rule as ContourTui/ScrollBar.qml's; separate styles, so stated in each.
    visible: control.policy === T.ScrollBar.AlwaysOn
             || (control.policy === T.ScrollBar.AsNeeded && control.size < 1.0 - 1e-6)

    // A thumb at least as long as the bar is wide, so it stays visible and grabbable: QQuickScrollBar
    // defaults this to 0 and would shrink the thumb of a long list to a sliver. The zero guards are for
    // the frame before the bar has a size.
    minimumSize: control.orientation === Qt.Horizontal
                 ? (control.width > 0 ? chromeStyle.size.scrollbar / control.width : 0)
                 : (control.height > 0 ? chromeStyle.size.scrollbar / control.height : 0)

    contentItem: Rectangle {
        implicitWidth: chromeStyle.size.scrollbar
        implicitHeight: chromeStyle.size.scrollbar
        radius: chromeStyle.radii.scrollbar
        color: control.pressed || control.hovered ? chromeStyle.colors.scrollbarThumbActive
                                                  : chromeStyle.colors.scrollbarThumb
        opacity: control.enabled ? 1.0 : 0.5
    }

    background: Rectangle {
        implicitWidth: chromeStyle.size.scrollbar
        implicitHeight: chromeStyle.size.scrollbar
        color: chromeStyle.colors.scrollbarTrack
    }
}
