// vim:syntax=qml
// The kit's ToolbarButton: a square glyph button in the title band, such as the band's new-tab "+".
//
// An Item with handlers rather than an AbstractButton, so its own `clicked` signal is the only one (a
// button root would already declare it). A left click is `clicked`; a right click or a press-and-hold is
// `secondaryClicked`, which is how the macOS "+" reaches the profiles (layout.profileMenu). Rest, hover
// and pressed follow the kit's states: transparent, colors.tabHoverFill (fill.3) and
// colors.controlPressedFill (fill.2), on a radii.control square of `extent` (size.toolbarButton unless the
// host is smaller, as a pane header's size.paneHeaderAction is).
import QtQuick

Item {
    id: root

    /// The glyph the button draws.
    property string glyph
    /// What assistive technology announces the button as.
    property string accessibleName
    /// What else the button does, such as what its secondary click opens; empty for nothing more.
    property string accessibleDescription
    /// The font the glyph is drawn in.
    property font glyphFont: chromeStyle.newTabFont
    /// The colour the glyph is drawn in: the band's tab text unless the host has a surface of its own.
    property color glyphColor: chromeStyle.colors.tabText
    /// The edge of the button's square.
    property real extent: chromeStyle.size.toolbarButton

    /// A left click, or the accessibility press action.
    signal clicked()
    /// A right click or a press-and-hold.
    signal secondaryClicked()

    Accessible.role: Accessible.Button
    Accessible.name: root.accessibleName
    Accessible.description: root.accessibleDescription
    Accessible.onPressAction: root.clicked()

    implicitWidth: root.extent
    implicitHeight: root.extent

    HoverHandler {
        id: hover
    }

    TapHandler {
        id: primary
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: root.clicked()
        // A long press is the secondary click a pointer without a second button gives; it then sends no tap.
        onLongPressed: root.secondaryClicked()
    }

    TapHandler {
        id: secondary
        acceptedButtons: Qt.RightButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: root.secondaryClicked()
    }

    Rectangle {
        id: background
        objectName: "toolbarButtonBackground"   // findChild() handle for the GUI test
        anchors.fill: parent
        radius: chromeStyle.radii.control
        color: primary.pressed || secondary.pressed ? chromeStyle.colors.controlPressedFill
             : hover.hovered ? chromeStyle.colors.tabHoverFill
             : "transparent"
    }

    Text {
        objectName: "toolbarButtonGlyph"   // findChild() handle for the GUI test
        anchors.centerIn: parent
        text: root.glyph
        font: root.glyphFont
        color: root.glyphColor
    }
}
