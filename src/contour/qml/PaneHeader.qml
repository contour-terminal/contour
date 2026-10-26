// vim:syntax=qml
// The kit's PaneHeader: a pane's program status, its name, its working directory and four action buttons
// (split right, split down, zoom, close), on a pane card once the tab is split. PaneCard.qml decides whether
// and where it shows; this draws what the provider's decision (`policy`, chromeStyle.paneHeader) says:
// the working directory only on a pane wide enough, the actions on hover (or as the row's rule says for a
// focused pane), the text's emphasis by focus.
//
// The buttons are the pointer's way to the pane actions, and take Tab focus while shown. The keyboard's
// way is the actions' own key bindings, which every button runs: SplitVertical (split right),
// SplitHorizontal (split down), TogglePaneZoom and ClosePane (window::PaneHeaderActions).
//
// The name is never elided before the working directory, which elides in the middle so that both the
// root and the leaf of a long path survive; assistive technology always hears the whole path.
//
// On the native rows the header is the card's own background (the row's header fill is transparent); on
// the Terminal row it is one cell row of the bar.
import QtQuick

Item {
    id: header
    objectName: "paneHeader"

    /// The TerminalSession of the pane this heads; null while a split rebinds.
    required property var session
    /// The PaneProxy whose actions the buttons run; null while a split rebinds.
    required property var node
    /// The provider's decision: `{ content, actions, placement, emphasis }` as their names.
    property var policy: ({})
    /// The card's corner radius: the header's top corners take it, its bottom edge stays square.
    property real cornerRadius: 0

    /// The pane's own background, which every colour here is resolved against.
    readonly property color background: session ? session.backgroundColor : "black"
    readonly property bool isOverlay: policy.placement === "overlay"
    readonly property string name: chromeStyle.paneHeaderName(session ? session.foregroundProcessName : "",
                                                               session ? session.title : "")
    readonly property string workingDirectory: session ? session.displayWorkingDirectory : ""
    readonly property int programStatus: session ? session.programStatus : chromeStyle.badgeKinds.none
    // colorOn() and badgeLook() read the palette, hence the paletteRevision read in each binding.
    readonly property var statusLook: chromeStyle.paletteRevision < 0 ? ({})
                                                                     : chromeStyle.badgeLook(programStatus)
    readonly property color dimmedText: chromeStyle.paletteRevision < 0 ? "transparent"
        : chromeStyle.colorOn(chromeStyle.roles.paneHeaderTextDimmed, background)
    readonly property color text: chromeStyle.paletteRevision < 0 ? "transparent"
        : policy.emphasis === "primary" ? chromeStyle.colorOn(chromeStyle.roles.paneHeaderText, background)
        : dimmedText

    // The layout's three spacings: the kit's leading and trailing padding, and the gap between items.
    readonly property real leading: chromeStyle.space.s5
    readonly property real trailing: chromeStyle.space.s3
    readonly property real gap: chromeStyle.space.paneHeaderGap

    Accessible.role: Accessible.StaticText
    // Concatenated, not formatted: a title the application set may itself contain "%2".
    Accessible.name: workingDirectory !== "" ? name + qsTr(", ") + workingDirectory : name

    // A click on the header, as on the terminal, makes the pane the active one. It takes the press for
    // itself: an overlay header lies over the terminal, which must not see the click as its own.
    TapHandler {
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: if (header.node) header.node.activate()
    }

    // The fill: the card's background (opaque as an overlay, at the profile's opacity inline, as the
    // terminal below it is) under the row's header fill. The rounded fill reaches one radius below the
    // header and is clipped there, so the top corners follow the card and the bottom edge meets the
    // terminal square.
    Item {
        anchors.fill: parent
        clip: header.cornerRadius > 0

        Rectangle {
            objectName: "paneHeaderBackground"
            anchors.fill: parent
            anchors.bottomMargin: -header.cornerRadius
            radius: header.cornerRadius
            color: header.background
            opacity: header.isOverlay || !header.session ? 1.0 : header.session.opacity
        }

        Rectangle {
            objectName: "paneHeaderFill"
            anchors.fill: parent
            anchors.bottomMargin: -header.cornerRadius
            radius: header.cornerRadius
            color: chromeStyle.paletteRevision < 0 ? "transparent"
                 : chromeStyle.colorOn(chromeStyle.roles.paneHeaderFill, header.background)
        }
    }

    Text {
        id: status
        objectName: "paneHeaderStatus"
        visible: header.programStatus !== chromeStyle.badgeKinds.none && text !== ""
        x: header.leading
        anchors.verticalCenter: parent.verticalCenter
        text: header.statusLook.glyph !== undefined ? header.statusLook.glyph : ""
        font: chromeStyle.type.paneHeader
        color: header.statusLook.ink !== undefined ? header.statusLook.ink : header.dimmedText
        Accessible.ignored: true
    }

    Text {
        id: nameLabel
        objectName: "paneHeaderName"
        x: status.visible ? status.x + status.width + header.gap : header.leading
        anchors.verticalCenter: parent.verticalCenter
        // Its whole width first: only a header too narrow for the name alone cuts it.
        width: Math.min(implicitWidth, Math.max(0, actions.x - header.gap - x))
        text: header.name
        elide: Text.ElideRight
        font: chromeStyle.type.paneHeader
        color: header.text
        Accessible.ignored: true
    }

    Text {
        id: cwdLabel
        objectName: "paneHeaderCwd"
        visible: header.policy.content === "nameAndCwd" && width > 0
        x: nameLabel.x + nameLabel.width + header.gap
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, actions.x - header.gap - x)
        text: header.workingDirectory
        elide: Text.ElideMiddle
        // The footnote face: the header's size on every row, at the regular weight the name does not take.
        font: chromeStyle.type.footnote
        color: header.dimmedText
        Accessible.ignored: true
    }

    Row {
        id: actions
        objectName: "paneHeaderActions"
        x: header.width - header.trailing - width
        anchors.verticalCenter: parent.verticalCenter
        spacing: header.gap
        // One fade, on every row: under Reduce Motion motion.fast is already the cross-fade's duration.
        opacity: header.policy.actions === "shown" ? 1.0 : 0.0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: chromeStyle.motion.fast } }

        Repeater {
            model: chromeStyle.paneActionGlyphs
            delegate: ToolbarButton {
                required property int index
                required property string modelData
                objectName: "paneHeaderAction" + index
                extent: chromeStyle.size.paneHeaderAction
                activeFocusOnTab: true
                glyph: modelData
                glyphFont: chromeStyle.type.paneHeaderAction
                glyphColor: header.text
                // By the pane's own tone, as the text is: the band's hover fill follows the chrome's.
                hoverFill: chromeStyle.paletteRevision < 0 ? "transparent"
                         : chromeStyle.colorOn(chromeStyle.roles.paneHeaderActionHover, header.background)
                accessibleName: chromeStyle.paneHeaderActionNames[index]
                onClicked: if (header.node) header.node.runHeaderAction(index)
            }
        }
    }
}
