// vim:syntax=qml
// The kit's HostChip: names the host a remote window is attached to (`contour client --host`) and how its
// connection stands -- a status dot, the host, and the state's text ("Reconnecting…", "Disconnected";
// nothing while connected). What each state looks like is UiStyleProvider::hostChipLook's table.
//
// A button: a click, a tap, or Space/Return while it holds the toolbar's keyboard focus emits `clicked`, which
// the title bar answers by opening the daemon & workspace switcher (WindowController.showSwitcher), since the
// host it names is what the switcher changes. Rest, hover and pressed follow the toolbar buttons' kit fills,
// and keyboard focus wears the focus ring. A local window has no host and the chip takes no room. On the
// Terminal row the chip is a run of cells, bracketed by the chip glyph tokens: `[● host status]`.
import QtQuick

Item {
    id: root
    objectName: "hostChip"

    /// A click, a tap, the accessibility press action, or Space/Return while focused.
    signal clicked()

    /// The attached host; empty for a local window.
    required property string hostLabel
    /// The connection's state, a window::HostChipState as its underlying value. Not `state`, which every
    /// Item already has (the States machinery's name).
    required property int connection
    /// Whether keyboard focus is on the chip and the chrome says so: the host's to set. The chip is static
    /// button, and it is the toolbar's first item in visual order, so the focus cycle visits it.
    property bool focusRingShown: false

    // How the state looks -- { dot, status, accessibleState } -- re-read on a palette change, since the
    // dot's ink follows it.
    readonly property var look: chromeStyle.paletteRevision < 0
                                ? ({}) : chromeStyle.hostChipLook(root.connection)
    readonly property string statusText: root.look.status !== undefined ? root.look.status : ""
    // Whether the chip is a run of bracketed cells (the Terminal row) rather than a rounded shape.
    readonly property bool bracketed: chromeStyle.chipOpenGlyph !== ""
    // The room between the dot, the host and the state: one cell on the Terminal row.
    readonly property real gap: root.bracketed ? chromeStyle.cellWidth : chromeStyle.space.s3

    Accessible.role: Accessible.Button
    Accessible.focusable: root.focusRingShown
    Accessible.focused: root.focusRingShown
    // The connection's state is named only while it is not the ordinary one: "(on work)",
    // "(on work, reconnecting)".
    Accessible.name: root.statusText === ""
                         ? qsTr("Switch daemon or workspace (on %1)").arg(root.hostLabel)
                         : qsTr("Switch daemon or workspace (on %1, %2)")
                               .arg(root.hostLabel).arg(root.look.accessibleState ?? "")
    Accessible.onPressAction: root.clicked()

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.clicked()
            event.accepted = true
        }
    }

    visible: chromeStyle.isHostChipShown(root.hostLabel)
    implicitWidth: root.visible
                   ? row.implicitWidth + (root.bracketed ? 0 : chromeStyle.space.s5 + chromeStyle.space.s5)
                   : 0
    implicitHeight: chromeStyle.size.hostChip
    width: root.implicitWidth
    height: root.implicitHeight

    HoverHandler {
        id: hover
        cursorShape: Qt.PointingHandCursor
    }

    TapHandler {
        id: primary
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: root.clicked()
    }

    Rectangle {
        objectName: "hostChipBackground"
        anchors.fill: parent
        radius: chromeStyle.radii.hostChip
        // The toolbar buttons' fills (ToolbarButton.qml), so the chip answers the pointer as they do.
        color: primary.pressed ? chromeStyle.colors.controlPressedFill
             : hover.hovered || root.activeFocus ? chromeStyle.colors.tabHoverFill
             : chromeStyle.colors.chipFill
    }

    // What a click does, with the chord that does the same.
    KitToolTipHost {
        text: qsTr("Switch daemon or workspace")
        shortcutCommand: "ShowSwitcher"
        toolTipName: "hostChipToolTip"   // findChild() handle for the GUI test
    }

    FocusRing {
        shown: root.focusRingShown
        hostRadius: chromeStyle.radii.hostChip
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 0

        Text {
            objectName: "hostChipOpen"
            visible: root.bracketed
            anchors.verticalCenter: parent.verticalCenter
            // The chip's own name says it all.
            Accessible.ignored: true
            text: chromeStyle.chipOpenGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.chipText
        }

        // The status dot: a vector circle on the native rows, the dot glyph on the Terminal row.
        Rectangle {
            id: dot
            objectName: "hostChipDot"
            visible: chromeStyle.hostDotGlyph === ""
            anchors.verticalCenter: parent.verticalCenter
            // The chip's own name says it all.
            Accessible.ignored: true
            width: chromeStyle.size.hostDot
            height: chromeStyle.size.hostDot
            radius: chromeStyle.size.hostDot
            color: root.look.dot !== undefined ? root.look.dot : "transparent"
        }

        Text {
            objectName: "hostChipDotGlyph"
            visible: chromeStyle.hostDotGlyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            // The chip's own name says it all.
            Accessible.ignored: true
            text: chromeStyle.hostDotGlyph
            font: chromeStyle.type.footnote
            color: dot.color
        }

        Text {
            objectName: "hostChipLabel"
            anchors.verticalCenter: parent.verticalCenter
            // The chip's own name says it all.
            Accessible.ignored: true
            // The room after the dot; padding, since a positioner skips an item without height.
            leftPadding: root.gap
            text: root.hostLabel
            font: chromeStyle.type.footnoteStrong
            color: chromeStyle.colors.chipTextStrong
        }

        // The state's text, absent while connected.
        Text {
            objectName: "hostChipStatus"
            visible: root.statusText !== ""
            anchors.verticalCenter: parent.verticalCenter
            // The chip's own name says it all.
            Accessible.ignored: true
            leftPadding: root.gap
            text: root.statusText
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.chipText
        }

        Text {
            objectName: "hostChipClose"
            visible: root.bracketed
            anchors.verticalCenter: parent.verticalCenter
            // The chip's own name says it all.
            Accessible.ignored: true
            text: chromeStyle.chipCloseGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.chipText
        }
    }
}
