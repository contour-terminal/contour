// vim:syntax=qml
// MenuItem for the ContourTui style. Highlight is inverse video rather than a tinted wash: that is how
// a terminal menu marks its cursor row, and it stays legible on every palette.
//
// A checkable item reserves a one-cell indicator column so the labels below it stay aligned -- the
// same reason a TUI menu pads its rows. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.MenuItem {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 0
    horizontalPadding: chromeStyle.labelPadding
    spacing: chromeStyle.labelGap
    font: chromeStyle.font

    indicator: Text {
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + ((control.availableHeight - height) / 2)
        visible: control.checkable
        width: chromeStyle.cellWidth
        text: control.checked ? "x" : " "
        font: control.font
        color: control.highlighted ? control.palette.highlightedText : control.palette.windowText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    // Marks a row that opens a sub-menu. Without it a sub-menu row is indistinguishable from one that
    // runs a command, and the only way to find out which it is, is to click it.
    arrow: Text {
        x: control.mirrored ? control.leftPadding : control.width - width - control.rightPadding
        y: control.topPadding + ((control.availableHeight - height) / 2)
        visible: control.subMenu
        width: chromeStyle.cellWidth
        text: chromeStyle.submenuGlyph
        font: control.font
        color: control.highlighted ? control.palette.highlightedText : control.palette.windowText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    contentItem: Text {
        // The indicator sits on the leading edge and the arrow on the trailing one, so which of them
        // a label has to make room for swaps with the reading direction.
        readonly property real indicatorRoom: control.checkable && control.indicator
                                              ? control.indicator.width + control.spacing : 0
        readonly property real arrowRoom: control.subMenu && control.arrow
                                          ? control.arrow.width + control.spacing : 0
        leftPadding: !control.mirrored ? indicatorRoom : arrowRoom
        rightPadding: control.mirrored ? indicatorRoom : arrowRoom
        text: control.text
        font: control.font
        color: control.highlighted ? control.palette.highlightedText : control.palette.windowText
        opacity: control.enabled ? 1.0 : 0.5
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: chromeStyle.cellWidth * 12
        implicitHeight: chromeStyle.controlHeight
        color: control.highlighted ? control.palette.highlight : "transparent"
    }
}
