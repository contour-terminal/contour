// vim:syntax=qml
// ItemDelegate for the ContourTui style: a menu row by another name, so it wears the same inverse
// highlight and the same one-cell insets. See ToolButton.qml for what a style file is.
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
    spacing: chromeStyle.labelGap
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
        implicitHeight: chromeStyle.controlHeight
        color: control.highlighted || control.down
               ? control.palette.highlight
               : control.hovered ? chromeStyle.wash(control.palette.highlight)
                                 : "transparent"
    }
}
