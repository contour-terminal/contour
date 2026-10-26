// vim:syntax=qml
// The kit's elevation beneath a surface: a soft nine-patch shadow when the level has a blur, otherwise (the
// Terminal row) a hard offset copy of the surface. Nothing at all when the blur and both offsets are zero.
//
// Fills the surface it lifts and sits below it (`anchors.fill: parent; z: -1` inside the surface, or a
// sibling beneath it). Toast lifts itself by e2, an active pane card by e1.
import QtQuick

Item {
    id: root

    /// The elevation level: one of chromeStyle.elevation's e0..e3.
    required property var level
    /// The corner radius of the surface it lifts.
    property real radius: 0

    readonly property color tint: chromeStyle.shadowTint(Qt.rgba(0, 0, 0, level.opacity))
    readonly property bool hasShadow: level.blur > 0 || level.offsetX !== 0 || level.offsetY !== 0
    // How far the soft shadow's nine-patch reaches beyond the surface.
    readonly property int reach: level.blur > 0 ? Math.max(1, Math.round((level.blur + 1) / 2)) * 3 : 0

    Rectangle {
        objectName: "elevationHardShadow"
        visible: root.hasShadow && root.level.blur <= 0
        x: root.level.offsetX
        y: root.level.offsetY
        width: root.width
        height: root.height
        radius: root.radius
        color: root.tint
    }

    BorderImage {
        objectName: "elevationSoftShadow"
        visible: root.hasShadow && root.level.blur > 0
        anchors.fill: parent
        anchors.margins: -root.reach
        transform: Translate { x: root.level.offsetX; y: root.level.offsetY }
        source: "image://popupshadow/" + Math.round(root.level.blur)
              + "/" + Math.round(root.level.offsetY)
              + "/" + Math.round(root.radius)
              + "/" + root.tint.toString().substring(1)
        border { left: root.reach + root.radius; top: root.reach + root.radius
                 right: root.reach + root.radius; bottom: root.reach + root.radius }
        horizontalTileMode: BorderImage.Stretch
        verticalTileMode: BorderImage.Stretch
    }
}
