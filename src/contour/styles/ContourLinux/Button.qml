// vim:syntax=qml
// Button for the ContourLinux style: a filled, rounded 34-pixel button, the kit's wash at rest, a
// stronger one on hover and press, and the accent for a checked or highlighted button. See
// ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T

T.Button {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: chromeStyle.space.s2
    horizontalPadding: chromeStyle.labelPadding
    spacing: chromeStyle.space.s3
    font: chromeStyle.font

    icon.width: chromeStyle.size.headerIcon
    icon.height: chromeStyle.size.headerIcon
    icon.color: control.checked || control.highlighted ? control.palette.highlightedText
                                                       : control.palette.buttonText

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.text
        font: control.font
        color: control.icon.color
        opacity: control.enabled ? 1.0 : 0.5
    }

    background: Rectangle {
        implicitWidth: chromeStyle.size.control
        implicitHeight: chromeStyle.size.control
        radius: chromeStyle.radii.control
        color: control.checked || control.highlighted ? control.palette.highlight
             : control.down ? chromeStyle.colors.controlPressedFill
             : control.hovered ? chromeStyle.colors.tabHoverFill
             : chromeStyle.colors.chipFill
        border.width: control.visualFocus ? chromeStyle.borderWidth : 0
        border.color: control.palette.highlight
        opacity: control.enabled ? 1.0 : 0.5
    }
}
