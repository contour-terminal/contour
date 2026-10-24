// vim:syntax=qml
// A full-width band that says one thing and offers up to two ways to answer it: a glyph, a word-wrapped
// message, an optional action button and a dismiss button.
//
// Placed by its user; it only fills the width it is given. It takes its fill, text and glyph colours,
// its insets and its font from the `chromeStyle` tokens, so under the Terminal row the band is the
// content's height (the vertical inset is zero there) in the scheme's own palette[0].
//
// It reports intent through signals and neither performs the action nor remembers the dismissal.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Control {
    id: root
    objectName: "banner"

    /// The message. An empty text hides the banner and collapses its height to zero.
    property string text
    /// The leading glyph.
    property string glyph: "!"
    /// The action button's label. Empty: no action button.
    property string actionText
    /// The dismiss button's label.
    property string dismissText: qsTr("Dismiss")

    signal actionTriggered()
    signal dismissed()

    visible: text.length > 0

    topPadding: chromeStyle.space.s3
    bottomPadding: chromeStyle.space.s3
    leftPadding: chromeStyle.space.s6
    rightPadding: chromeStyle.space.s6

    // Bound explicitly: the active style's Control may supply no implicit sizes (ContourTui has none).
    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: implicitContentHeight + topPadding + bottomPadding
    height: visible ? implicitHeight : 0

    Accessible.role: Accessible.AlertMessage
    Accessible.name: root.text

    background: Rectangle {
        color: chromeStyle.colors.bannerFill

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: chromeStyle.borderWidth
            color: Qt.alpha(chromeStyle.colors.bannerText, chromeStyle.contrast.separatorAlpha)
        }
    }

    contentItem: RowLayout {
        spacing: chromeStyle.space.s5

        Label {
            objectName: "bannerGlyph"
            text: root.glyph
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.bannerGlyph
            Accessible.ignored: true
        }

        Label {
            objectName: "bannerText"
            Layout.fillWidth: true
            text: root.text
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.bannerText
            wrapMode: Text.Wrap
        }

        Button {
            objectName: "bannerAction"
            visible: root.actionText.length > 0
            text: root.actionText
            font: chromeStyle.type.footnote
            Accessible.name: text
            onClicked: root.actionTriggered()
        }

        Button {
            objectName: "bannerDismiss"
            text: root.dismissText
            font: chromeStyle.type.footnote
            Accessible.name: text
            onClicked: root.dismissed()
        }
    }
}
