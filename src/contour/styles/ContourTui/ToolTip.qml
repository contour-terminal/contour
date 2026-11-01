// vim:syntax=qml
// ToolTip for the ContourTui style: any attached tooltip (the attached one is the stray case; the kit's own
// controls use qml/KitToolTip.qml) drawn from the same tooltip tokens, below its parent. Opaque for the
// reason Popup.qml states. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T
import Contour.Ui

T.ToolTip {
    id: control

    x: parent ? (parent.width - implicitWidth) / 2 : 0
    y: parent ? parent.height + chromeStyle.space.s2 : 0

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 0
    leftPadding: chromeStyle.size.tooltipPadX
    rightPadding: chromeStyle.size.tooltipPadX
    topPadding: chromeStyle.size.tooltipPadTop
    bottomPadding: chromeStyle.size.tooltipPadBottom

    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        text: control.text
        // Never AutoText: a tooltip's text may be what an application wrote (a link's URI, a tab title, a
        // command line), and AutoText renders it as rich text whenever its first line looks like markup.
        textFormat: Text.PlainText
        font: chromeStyle.type.footnote
        color: chromeStyle.colors.tooltipText
        wrapMode: Text.WordWrap
    }

    background: Rectangle {
        radius: chromeStyle.radii.tooltip
        color: chromeStyle.colors.tooltipFill
        border.width: chromeStyle.colors.tooltipEdge.a > 0 ? (chromeStyle.size.tooltipEdge > 0 ? chromeStyle.size.tooltipEdge : chromeStyle.borderWidth) : 0
        border.color: chromeStyle.colors.tooltipEdge

        ElevationShadow {
            anchors.fill: parent
            z: -1
            level: [chromeStyle.elevation.e0, chromeStyle.elevation.e1, chromeStyle.elevation.e2, chromeStyle.elevation.e3][chromeStyle.size.tooltipElevation]
            radius: parent.radius
        }
    }
}
