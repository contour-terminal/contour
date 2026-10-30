// vim:syntax=qml
// ToolTip for the ContourLinux style: a rounded box of system-font text above its parent. Opaque for the
// reason Popup.qml states. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.ToolTip {
    id: control

    x: parent ? (parent.width - implicitWidth) / 2 : 0
    y: -implicitHeight - chromeStyle.space.s2

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: chromeStyle.space.s2
    horizontalPadding: chromeStyle.space.s4

    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        text: control.text
        // Never AutoText: a tooltip's text may be what an application wrote (a link's URI, a tab title, a
        // command line), and AutoText renders it as rich text whenever its first line looks like markup.
        textFormat: Text.PlainText
        font: chromeStyle.font
        color: control.palette.toolTipText
        wrapMode: Text.WordWrap
    }

    background: Rectangle {
        radius: chromeStyle.radii.control
        color: control.palette.toolTipBase
        border.width: chromeStyle.borderWidth
        border.color: control.palette.mid
    }
}
