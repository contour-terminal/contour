// vim:syntax=qml
// A freedesktop symbolic icon in one colour: a button's contentItem on the Linux header bar.
//
// It draws the button's `icon` group -- the theme's icon by `icon.name`, else the bundled SVG copy at
// `icon.source` (qrc:/contour/icons/, loaded through Qt SVG) -- recoloured to `icon.color`. IconImage
// paints the colour over the icon's own coverage (QPainter's SourceIn), so every pixel takes the colour's
// hue and only the icon's shape survives: what the theme or the file was drawn in never shows, and the
// icon follows the light, dark and contrast themes through the colour role the button names.
//
// Set as the contentItem rather than left to the Quick Controls style, so the tint does not depend on
// which style the application runs under.
import QtQuick
import QtQuick.Controls.impl

IconImage {
    id: root

    /// The icon to draw: the owning button's `icon` group (name, source, color, width, height).
    required property var icon

    name: root.icon.name
    source: root.icon.source
    color: root.icon.color
    sourceSize: Qt.size(root.icon.width, root.icon.height)
}
