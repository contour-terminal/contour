// vim:syntax=qml
// CheckBox for the ContourLinux style: a rounded box the size of a header icon, filled with the accent
// and marked when checked. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.CheckBox {
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
        readonly property bool marked: control.checkState !== Qt.Unchecked
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + ((control.availableHeight - height) / 2)
        implicitWidth: chromeStyle.size.headerIcon
        implicitHeight: chromeStyle.size.headerIcon
        radius: chromeStyle.radii.tabIcon
        color: marked ? control.palette.highlight : control.palette.base
        border.width: marked ? 0 : chromeStyle.borderWidth
        border.color: control.visualFocus ? control.palette.highlight : control.palette.mid
        opacity: control.enabled ? 1.0 : 0.5

        Text {
            anchors.centerIn: parent
            visible: parent.marked
            text: control.checkState === Qt.PartiallyChecked ? "–" : "✓"
            font: chromeStyle.captionFont
            color: control.palette.highlightedText
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
