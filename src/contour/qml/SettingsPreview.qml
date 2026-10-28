// vim:syntax=qml
// The hero of a profile page: a live terminal over a canned transcript, drawn with the profile as the page
// has it, and the caption naming its font, size and scheme beneath it.
//
// The terminal is the SettingsPreview's own session (window/SettingsPreview.hpp), which follows the
// controller's drafts, so every change shows here before it is written. It takes no input: it is disabled,
// out of the Tab order, and a screen reader hears one picture named by the caption. Without a preview (the
// snapshot scene has none) the frame shows empty and nothing is printed.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Contour.Terminal

Column {
    id: root

    /// The window's SettingsPreview; null where the host has none.
    property var preview: null
    /// The profile to show. Asking the preview for it is this item's business, so the page need not.
    property string profileName: ""
    /// Whether the preview shows the profile's status line, forced on, rather than as the profile has it.
    property bool statusLine: false
    /// What sits at the caption's trailing edge: the page's buttons for the profile.
    property alias trailing: trailingRow.data

    // The frame is the style's width, or the whole page's where the style gives none.
    readonly property bool _fixedWidth: chromeStyle.settings.previewWidth > 0

    spacing: chromeStyle.space.s3

    // Shown again whenever the page opens another profile.
    function show() {
        if (!root.preview || root.profileName.length === 0)
            return;
        if (root.statusLine)
            root.preview.showStatusLine(root.profileName);
        else
            root.preview.show(root.profileName);
    }
    onStatusLineChanged: root.show()
    onPreviewChanged: root.show()
    onProfileNameChanged: root.show()
    Component.onCompleted: root.show()

    Rectangle {
        id: frame
        objectName: "settingsPreview"
        width: root._fixedWidth ? Math.min(chromeStyle.settings.previewWidth, root.width) : root.width
        height: chromeStyle.settings.previewHeight
        radius: chromeStyle.settings.previewRadius
        color: "transparent"
        border.width: chromeStyle.borderWidth
        border.color: chromeStyle.colors.previewFrame
        enabled: false
        activeFocusOnTab: false

        Accessible.role: Accessible.Graphic
        Accessible.name: root.preview ? root.preview.caption : ""

        ContourTerminal {
            anchors.fill: parent
            anchors.margins: chromeStyle.borderWidth
            session: root.preview ? root.preview.session : null
            activeFocusOnTab: false
        }
    }

    RowLayout {
        width: parent.width
        spacing: chromeStyle.space.s6

        Label {
            objectName: "settingsPreviewCaption"
            Layout.fillWidth: true
            text: root.preview ? root.preview.caption : ""
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.settingsHelp
            elide: Text.ElideRight
            textFormat: Text.PlainText
            Accessible.ignored: true
        }

        Row {
            id: trailingRow
            spacing: chromeStyle.space.s3
        }
    }
}
