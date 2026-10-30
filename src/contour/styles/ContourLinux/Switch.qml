// vim:syntax=qml
// Switch for the ContourLinux style: a pill track, the accent when on, with a round thumb that slides
// to the trailing end. The track is a window button tall and a control plus an icon wide (24 by 50,
// GTK's own proportions); the thumb is inset by s1 on every side. See ToolButton.qml for what a style
// file is.
import QtQuick
import QtQuick.Templates as T

T.Switch {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: chromeStyle.space.s2
    spacing: chromeStyle.space.s4
    font: chromeStyle.font

    indicator: Rectangle {
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + ((control.availableHeight - height) / 2)
        implicitWidth: chromeStyle.size.control + chromeStyle.size.headerIcon
        implicitHeight: chromeStyle.size.windowButton
        // Larger than half the height on purpose: a Rectangle clamps its radius to that, which is a pill.
        radius: height
        color: control.checked ? control.palette.highlight : chromeStyle.colors.controlPressedFill
        border.width: control.visualFocus ? chromeStyle.borderWidth : 0
        border.color: control.palette.highlight
        opacity: control.enabled ? 1.0 : 0.5

        Rectangle {
            readonly property real inset: chromeStyle.space.s1
            x: control.checked !== control.mirrored ? parent.width - width - inset : inset
            y: inset
            width: parent.height - chromeStyle.space.s2
            height: width
            radius: width
            color: control.palette.base

            Behavior on x {
                NumberAnimation { duration: chromeStyle.motion.fast }
            }
        }
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: control.palette.windowText
        opacity: control.enabled ? 1.0 : 0.5
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
