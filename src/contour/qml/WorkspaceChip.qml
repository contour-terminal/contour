// vim:syntax=qml
// The workspace chip, leading the tab strip while the sidebar is hidden: it names the workspace the window
// shows (`billing ▾`) and, as the pop-up button it reads as, drops the window's workspaces down below itself
// (WindowController.showWorkspaceDropdown). While another workspace needs the user, or has a result they have not seen, it carries that
// workspace's status glyph as a marker (SidebarModel.workspaceChipMarked, workspaceChipGlyph) -- a shape, so it
// does not rest on colour alone. Rest, hover and pressed follow the toolbar buttons' kit fills; keyboard focus
// wears the focus ring; on the Terminal row it is a run of bracketed cells, `[billing ▾ !]`.
import QtQuick

Item {
    id: root
    objectName: "workspaceChip"

    /// A click, a tap, the accessibility press action, or Space/Return while focused.
    signal clicked()

    /// The window's sidebar model, which names the workspace and decides the marker.
    required property var sidebar
    /// Whether keyboard focus is on the chip and the chrome says so: the host's to set.
    property bool focusRingShown: false

    readonly property string label: root.sidebar ? root.sidebar.currentWorkspaceTitle : ""
    readonly property bool marked: !!root.sidebar && root.sidebar.workspaceChipMarked
    readonly property bool bracketed: chromeStyle.chipOpenGlyph !== ""
    readonly property real gap: root.bracketed ? chromeStyle.cellWidth : chromeStyle.space.s3

    Accessible.role: Accessible.ButtonMenu
    Accessible.focusable: root.focusRingShown
    Accessible.focused: root.focusRingShown
    Accessible.name: root.sidebar ? root.sidebar.workspaceChipAccessibleName : ""
    Accessible.onPressAction: root.clicked()

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.clicked()
            event.accepted = true
        }
    }

    implicitWidth: row.implicitWidth + (root.bracketed ? 0 : chromeStyle.space.s5 + chromeStyle.space.s5)
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

    /// The chip in the scene, with the gap a dropdown hangs below it by: where its dropdown opens.
    function dropdownAnchor() {
        return root.mapToItem(null, 0, 0, root.width, root.height + chromeStyle.space.s1)
    }

    Rectangle {
        anchors.fill: parent
        radius: chromeStyle.radii.hostChip
        color: chromeStyle.paletteRevision < 0 ? "transparent"
             : primary.pressed ? chromeStyle.colors.controlPressedFill
             : hover.hovered || root.activeFocus ? chromeStyle.colors.tabHoverFill
             : chromeStyle.colors.chipFill
    }

    KitToolTipHost {
        text: qsTr("Switch workspace")
        toolTipName: "workspaceChipToolTip"
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
            visible: root.bracketed
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            text: chromeStyle.chipOpenGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.chipText
        }

        Text {
            objectName: "workspaceChipLabel"
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            textFormat: Text.PlainText
            text: root.label
            font: chromeStyle.type.footnoteStrong
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.chipTextStrong
        }

        // The pop-up button's caret, a small step after the name (a cell on the Terminal row).
        Text {
            objectName: "workspaceChipCaret"
            anchors.verticalCenter: parent.verticalCenter
            leftPadding: root.bracketed ? chromeStyle.cellWidth : chromeStyle.space.s1
            Accessible.ignored: true
            textFormat: Text.PlainText
            text: chromeStyle.menuGlyph
            font: chromeStyle.type.footnoteStrong
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.chipTextStrong
        }

        // The marker: another workspace's status glyph, after a gap.
        Item {
            visible: marker.known && root.marked
            anchors.verticalCenter: parent.verticalCenter
            width: root.gap + marker.implicitWidth
            height: marker.implicitHeight

            TabBadgeSlot {
                id: marker
                objectName: "workspaceChipMarker"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                kind: root.sidebar ? root.sidebar.workspaceChipGlyph : 0
                shaped: !root.bracketed
                Accessible.ignored: true
            }
        }

        Text {
            visible: root.bracketed
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            text: chromeStyle.chipCloseGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.chipText
        }
    }
}
