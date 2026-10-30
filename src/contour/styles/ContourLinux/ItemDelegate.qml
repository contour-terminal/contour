// vim:syntax=qml
// ItemDelegate for the ContourLinux style: a list row with a menu row's rounded highlight, inset from
// the list's edges. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.ItemDelegate {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 0
    horizontalPadding: chromeStyle.labelPadding
    spacing: chromeStyle.space.s3
    leftInset: chromeStyle.space.s1
    rightInset: chromeStyle.space.s1
    font: chromeStyle.font

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.highlighted || control.down ? control.palette.highlightedText
                                                   : control.palette.text
        opacity: control.enabled ? 1.0 : 0.5
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitHeight: chromeStyle.size.control
        radius: chromeStyle.radii.menuRow
        color: control.highlighted || control.down ? control.palette.highlight
             : control.hovered ? chromeStyle.colors.tabHoverFill
             : "transparent"
    }
}
