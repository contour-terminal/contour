// vim:syntax=qml
// Button for the ContourTui style: a square, single-row box drawn with a hairline border and one cell
// of padding either side. Pressed reads as inverse video, which is how a terminal shows an active
// field. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.Button {
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
        color: control.down || control.checked ? control.palette.highlightedText
                                               : control.palette.buttonText
        opacity: control.enabled ? 1.0 : 0.5
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: chromeStyle.cellWidth * 10
        implicitHeight: chromeStyle.controlHeight
        radius: chromeStyle.radius
        color: control.down || control.checked ? control.palette.highlight : "transparent"
        border.width: chromeStyle.borderWidth
        border.color: control.visualFocus ? control.palette.highlight : control.palette.mid
        opacity: control.enabled ? 1.0 : 0.5
    }
}
