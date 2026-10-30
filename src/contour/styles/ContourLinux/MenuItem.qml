// vim:syntax=qml
// MenuItem for the ContourLinux style: a 34-pixel row whose highlight is the accent with the menu row's
// rounded corners, inset from the menu's edge by the menu's own padding.
//
// A checkable item reserves an indicator column so the labels below it stay aligned, and a sub-menu row
// carries an arrow, as ContourTui's do. In-scene menus show no chords. See ToolButton.qml for what a
// style file is.
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
    spacing: chromeStyle.space.s3
    font: chromeStyle.font

    indicator: Text {
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + ((control.availableHeight - height) / 2)
        visible: control.checkable
        width: chromeStyle.size.headerIcon
        text: control.checked ? "✓" : ""
        font: control.font
        color: control.highlighted ? control.palette.highlightedText : control.palette.windowText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    // Marks a row that opens a sub-menu: without it a sub-menu row reads like one that runs a command.
    arrow: Text {
        x: control.mirrored ? control.leftPadding : control.width - width - control.rightPadding
        y: control.topPadding + ((control.availableHeight - height) / 2)
        visible: control.subMenu
        width: chromeStyle.size.headerIcon
        text: chromeStyle.submenuGlyph
        font: control.font
        color: control.highlighted ? control.palette.highlightedText : control.palette.windowText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    contentItem: Text {
        // The indicator sits on the leading edge and the arrow on the trailing one, so which of them a
        // label makes room for swaps with the reading direction.
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
        implicitHeight: chromeStyle.size.control
        radius: chromeStyle.radii.menuRow
        color: control.highlighted ? control.palette.highlight : "transparent"
    }
}
