// vim:syntax=qml
// ToolTip for the ContourTui style: a bordered box of chrome-font text, one cell of padding.
// Opaque for the same reason Popup.qml is. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.ToolTip {
    id: control

    x: parent ? (parent.width - implicitWidth) / 2 : 0
    y: -implicitHeight - chromeStyle.borderWidth

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 0
    horizontalPadding: chromeStyle.labelPadding

    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        text: control.text
        font: chromeStyle.font
        color: control.palette.toolTipText
        wrapMode: Text.WordWrap
    }

    background: Rectangle {
        radius: chromeStyle.radius
        color: control.palette.toolTipBase
        border.width: chromeStyle.borderWidth
        border.color: control.palette.mid
    }
}
