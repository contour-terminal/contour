// vim:syntax=qml
// A transient notice: a short text with an optional leading glyph, shown as a pill (or, under the Terminal
// row, a one-row strip of the scheme's foreground on its background).
//
// Presentational. Its host positions it and decides when it shows; it only fades and slides in and out
// as `shown` changes. Its height, radius, insets, font, colours, motion and shadow all come from the
// `chromeStyle` tokens, so this file never learns which style is active.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Control {
    id: root
    objectName: "toast"

    /// The message.
    property string text
    /// The optional leading glyph. Empty: none.
    property string glyph
    /// Whether the toast is showing. QML-facing, so a host binds it directly.
    property bool shown: false

    /// How far the toast slides from while entering; zero when motion is reduced.
    readonly property real enterOffset: chromeStyle.motion.travel

    // Bound explicitly: the active style's Control may supply no implicit sizes (ContourTui has none).
    height: chromeStyle.size.toast
    implicitHeight: chromeStyle.size.toast
    leftPadding: chromeStyle.space.s7
    rightPadding: chromeStyle.space.s7
    implicitWidth: implicitContentWidth + leftPadding + rightPadding

    opacity: shown ? 1 : 0
    Behavior on opacity {
        NumberAnimation { duration: chromeStyle.motion.standard }
    }

    transform: Translate {
        y: root.shown ? 0 : root.enterOffset
        Behavior on y {
            NumberAnimation { duration: chromeStyle.motion.standard }
        }
    }

    Accessible.role: Accessible.AlertMessage
    Accessible.name: root.text

    background: Rectangle {
        color: chromeStyle.colors.toastFill
        radius: chromeStyle.radii.toast

        // The elevation: a soft nine-patch shadow when there is blur, otherwise (the Terminal row) a hard
        // offset copy of the surface. No shadow at all when blur and both offsets are zero.
        readonly property var _lift: chromeStyle.elevation.e2
        readonly property color _tint: chromeStyle.shadowTint(Qt.rgba(0, 0, 0, _lift.opacity))
        readonly property bool _hasShadow: _lift.blur > 0 || _lift.offsetX !== 0 || _lift.offsetY !== 0
        readonly property int _reach: _lift.blur > 0
                                      ? Math.max(1, Math.round((_lift.blur + 1) / 2)) * 3
                                      : 0

        Rectangle {
            visible: parent._hasShadow && parent._lift.blur <= 0
            z: -1
            x: parent._lift.offsetX
            y: parent._lift.offsetY
            width: parent.width
            height: parent.height
            radius: parent.radius
            color: parent._tint
        }

        BorderImage {
            visible: parent._hasShadow && parent._lift.blur > 0
            z: -1
            anchors.fill: parent
            anchors.margins: -parent._reach
            transform: Translate { x: chromeStyle.elevation.e2.offsetX; y: chromeStyle.elevation.e2.offsetY }
            source: "image://popupshadow/" + Math.round(parent._lift.blur)
                  + "/" + Math.round(parent._lift.offsetY)
                  + "/" + Math.round(parent.radius)
                  + "/" + parent._tint.toString().substring(1)
            border { left: parent._reach + parent.radius; top: parent._reach + parent.radius
                     right: parent._reach + parent.radius; bottom: parent._reach + parent.radius }
            horizontalTileMode: BorderImage.Stretch
            verticalTileMode: BorderImage.Stretch
        }
    }

    contentItem: RowLayout {
        spacing: chromeStyle.space.s5

        Label {
            objectName: "toastGlyph"
            visible: root.glyph.length > 0
            Layout.alignment: Qt.AlignVCenter
            text: root.glyph
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.toastText
            Accessible.ignored: true
        }

        Label {
            objectName: "toastText"
            Layout.alignment: Qt.AlignVCenter
            text: root.text
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.toastText
        }
    }
}
