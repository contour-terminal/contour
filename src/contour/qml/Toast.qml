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
    /// A second, quieter text after the message (the link pill's hint). Empty: none.
    property string detail
    /// The optional leading glyph. Empty: none.
    property string glyph
    /// Whether the toast is showing. QML-facing, so a host binds it directly.
    property bool shown: false
    /// The glyph's colour: the text's unless the glyph is a mark with an ink of its own.
    property color glyphColor: chromeStyle.colors.toastText
    /// The elevation it lifts itself by: e2 as a transient notice; a host showing it as a steady state
    /// in its own surface may lower it.
    property var elevation: chromeStyle.elevation.e2

    /// Its height: the toast's by default; a pill of another extent (the link pill) sets its own.
    property real extent: chromeStyle.size.toast
    /// Its corner radius.
    property real cornerRadius: chromeStyle.radii.toast
    /// Its leading and trailing padding.
    property real inset: chromeStyle.space.s7
    /// The font both texts are set in.
    property font textFont: chromeStyle.type.footnote
    /// How the text is cut where the toast is given less width than it asks for: not at all by default; the
    /// link pill cuts a long target in its middle, where both its host and its last segment survive.
    property int textElide: Text.ElideNone

    /// How far the toast slides from while entering; zero when motion is reduced.
    readonly property real enterOffset: chromeStyle.motion.travel

    // Bound explicitly: the active style's Control may supply no implicit sizes (ContourTui has none).
    height: root.extent
    implicitHeight: root.extent
    leftPadding: root.inset
    rightPadding: root.inset
    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    // The gap between its glyph, its text and its detail (Control's own spacing): s5, or a host's.
    spacing: chromeStyle.space.s5

    opacity: shown ? 1 : 0
    // Gone once it has faded out: a host keeps it in the scene for good (Main.qml, over the band's reveal
    // edge in full screen), and a Control left there invisible still takes the pointer's hover from what
    // lies beneath it.
    visible: opacity > 0
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
    Accessible.name: root.detail.length > 0 ? root.text + ", " + root.detail : root.text

    background: Rectangle {
        color: chromeStyle.colors.toastFill
        radius: root.cornerRadius

        // The elevation (ElevationShadow): soft where there is blur, a hard offset copy on the Terminal row.
        ElevationShadow {
            anchors.fill: parent
            z: -1
            level: root.elevation
            radius: parent.radius
        }
    }

    contentItem: RowLayout {
        spacing: root.spacing

        Label {
            objectName: "toastGlyph"
            visible: root.glyph.length > 0
            Layout.alignment: Qt.AlignVCenter
            text: root.glyph
            font: root.textFont
            color: root.glyphColor
            Accessible.ignored: true
        }

        Label {
            objectName: "toastText"
            Layout.alignment: Qt.AlignVCenter
            // Its implicit width stays the whole text's, which is what a host places the toast by; only a
            // width given from outside makes it cut.
            Layout.fillWidth: root.textElide !== Text.ElideNone
            Layout.minimumWidth: 0
            elide: root.textElide
            text: root.text
            // What a toast says can quote a name or an error a program or a file supplied, or a link's target an
            // application chose: text, never markup.
            textFormat: Text.PlainText
            font: root.textFont
            color: chromeStyle.colors.toastText
        }

        Label {
            objectName: "toastDetail"
            visible: root.detail.length > 0
            Layout.alignment: Qt.AlignVCenter
            text: root.detail
            textFormat: Text.PlainText
            font: root.textFont
            color: chromeStyle.colors.toastTextSecondary
            Accessible.ignored: true
        }
    }
}
