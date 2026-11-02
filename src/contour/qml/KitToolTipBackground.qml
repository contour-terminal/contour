// vim:syntax=qml
// The box of a tooltip: the row's fill, edge, corner radius and e2 shadow, all from the chrome tokens. Shared
// by KitToolTip and the styles' ToolTip, so every tooltip is drawn alike.
import QtQuick
import QtQuick.Window

Rectangle {
    id: root
    objectName: "kitToolTipBackground"

    /// How far beyond the box its shadow reaches: the popup keeps this much clear of the window's edge.
    readonly property real shadowReach: shadow.reach + Math.max(Math.abs(shadow.level.offsetX),
                                                                Math.abs(shadow.level.offsetY))

    radius: chromeStyle.radii.tooltip
    color: chromeStyle.colors.tooltipFill
    border.width: chromeStyle.tooltipEdgeWidth(Screen.devicePixelRatio)
    border.color: chromeStyle.colors.tooltipEdge

    ElevationShadow {
        id: shadow
        anchors.fill: parent
        z: -1
        level: chromeStyle.elevation.e2
        radius: root.radius
    }
}
