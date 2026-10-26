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
    /// The glyph's colour: the text's unless the glyph is a mark with an ink of its own.
    property color glyphColor: chromeStyle.colors.toastText
    /// The elevation it lifts itself by: e2 as a transient notice; a host showing it as a steady state
    /// in its own surface may lower it.
    property var elevation: chromeStyle.elevation.e2

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

        // The elevation (ElevationShadow): soft where there is blur, a hard offset copy on the Terminal row.
        ElevationShadow {
            anchors.fill: parent
            z: -1
            level: root.elevation
            radius: parent.radius
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
            color: root.glyphColor
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
