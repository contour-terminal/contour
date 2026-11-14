// vim:syntax=qml
// The location chip, leading the band: it names the daemon the window lives in and the workspace it shows --
// `local › billing ▾` -- and, as the pop-up button it reads as, drops the switcher down below itself
// (WindowController.showWorkspaceDropdown). A remote daemon, or a connection that is not up, adds its status
// dot (a glyph on the Terminal row, one per state) and, while there is room, the state's words
// (`● starfive Reconnecting… › billing ▾`). While another workspace needs the user, or has a result they have
// not seen, it carries that workspace's status glyph as a marker (SidebarModel.workspaceChipMarked,
// workspaceChipGlyph) -- a shape, so it does not rest on colour alone. An in-process window names no daemon
// and the chip is the workspace alone.
//
// Every decision -- the daemon's name, whether the dot shows, which fit step the room allows, what each step
// says, the tooltip, the accessible words -- is the controller's (window/LocationChip.hpp). The chip measures
// its steps (`fitWidths`) and lays out whichever `fit` the title bar sets from the room it computes. Rest,
// hover and pressed follow the toolbar buttons' kit fills; keyboard focus wears the focus ring; on the
// Terminal row it is a run of bracketed cells whose widths are whole cells.
import QtQuick

Item {
    id: root
    objectName: "workspaceChip"

    /// A click, a tap, the accessibility press action, or Space/Return while focused.
    signal clicked()

    /// The window's sidebar model, which names the workspace and decides the marker.
    required property var sidebar
    /// The window's controller, which decides everything else the chip says (WindowController's chip* members).
    required property var controller
    /// How much of the chip fits, a window::LocationChipFit as its underlying value: the title bar's to set,
    /// from `fitWidths` and the room it leaves (never from the chip's own width, which would loop).
    property int fit: 0
    /// Whether keyboard focus is on the chip and the chrome says so: the host's to set.
    property bool focusRingShown: false
    /// Whether the dropdown the chip opened is open: the chip wears its pressed fill meanwhile.
    property bool isOpen: false
    // Whether the dropdown was open as the press began: that press closed it, and its release must not open
    // it again (ui-ux rule 2).
    property bool _wasOpenAtPress: false

    // What makes the controller's answers about the chip stale: its daemon, the connection's state glyph and
    // the dot's decision. Read inside every binding that calls one of its invokables, which QML cannot track.
    readonly property string _revision: root.controller.chipDaemonName + "|" + root.controller.chipStateGlyph
                                        + "|" + root.controller.chipDaemonDot
    readonly property string workspaceName: root.sidebar ? root.sidebar.currentWorkspaceTitle : ""
    readonly property bool marked: !!root.sidebar && root.sidebar.workspaceChipMarked
    readonly property bool bracketed: chromeStyle.chipOpenGlyph !== ""
    readonly property real gap: root.bracketed ? chromeStyle.cellWidth : chromeStyle.space.s3
    /// Whether the status dot (or, bracketed, its state glyph) shows: the controller's call, zero meaning no.
    readonly property bool dotShown: root.controller.chipDaemonName !== "" && root.controller.chipDaemonDot !== 0
    // How the connection looks: the dot's ink follows the palette.
    readonly property var look: chromeStyle.paletteRevision < 0
                                ? ({}) : chromeStyle.hostChipLook(root.controller.hostChipState ?? 0)
    readonly property color dotInk: root.look.dot !== undefined ? root.look.dot : "transparent"
    /// What the daemon segment, the state and the workspace say at the current fit.
    readonly property string daemonText: { root._revision; return root.controller.chipDaemonText(root.fit) }
    readonly property string statusText: { root._revision; return root.controller.chipStatusText(root.fit) }
    readonly property string workspaceText: {
        root._revision
        return root.controller.chipWorkspaceText(root.workspaceName, root.fit)
    }
    /// The fit steps `fitWidths` measures, each { fit, key }, in elision order: the controller's to say
    /// (WindowController.chipFitSteps), so the order and the keys have one source.
    readonly property var fitSteps: root.controller.chipFitSteps
    /// What each fit step needs, by the keys WindowController.chipFit takes, in pixels (whole cells on the
    /// Terminal row). Independent of `fit`, so the title bar can choose `fit` from it.
    readonly property var fitWidths: {
        root._revision
        fontMetrics.font
        strongMetrics.font
        const widths = {}
        root.fitSteps.forEach(step => { widths[step.key] = root._widthAt(step.fit) })
        return widths
    }

    FontMetrics {
        id: fontMetrics
        font: chromeStyle.type.footnote
    }

    FontMetrics {
        id: strongMetrics
        font: chromeStyle.type.footnoteStrong
    }

    // A text's width: whole cells on the Terminal row.
    function _span(metrics, text) {
        const width = metrics.advanceWidth(text)
        return root.bracketed ? Math.ceil(width / chromeStyle.cellWidth) * chromeStyle.cellWidth : width
    }

    // What the chip needs at one fit step: what stays whatever fits (the ends, the caret and the marker) and
    // the dot, the daemon, the state and the workspace as that step says them.
    function _widthAt(step) {
        const daemon = root.controller.chipDaemonText(step)
        const status = root.controller.chipStatusText(step)
        const workspace = root.controller.chipWorkspaceText(root.workspaceName, step)
        let width = root.bracketed ? root._span(fontMetrics, chromeStyle.chipOpenGlyph + chromeStyle.chipCloseGlyph)
                                   : 2 * chromeStyle.space.s5
        width += caret.leftPadding + root._span(strongMetrics, chromeStyle.menuGlyph)
        width += markerSlot.visible ? markerSlot.width : 0
        let leading = false
        if (root.dotShown) {
            width += root.bracketed ? root._span(fontMetrics, root.controller.chipStateGlyph) : chromeStyle.size.hostDot
            leading = true
        }
        if (daemon !== "") {
            width += (leading ? root.gap : 0) + root._span(fontMetrics, daemon)
            leading = true
        }
        if (status !== "")
            width += root.gap + root._span(fontMetrics, status)
        if (daemon !== "")
            width += root.gap + root._span(fontMetrics, chromeStyle.breadcrumbGlyph)
        return width + (leading ? root.gap : 0) + root._span(strongMetrics, workspace)
    }

    Accessible.role: Accessible.ButtonMenu
    Accessible.focusable: root.focusRingShown
    Accessible.focused: root.focusRingShown
    Accessible.name: root.controller.chipAccessibleName
    Accessible.description: root.controller.chipAccessibleDescription
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
        onPressedChanged: if (primary.pressed) root._wasOpenAtPress = root.isOpen
        onTapped: if (!root._wasOpenAtPress) root.clicked()
    }

    /// The chip in the scene: what its dropdown hangs below (the gap is the dropdown's).
    function dropdownAnchor() {
        return root.mapToItem(null, 0, 0, root.width, root.height)
    }

    Rectangle {
        anchors.fill: parent
        radius: chromeStyle.radii.hostChip
        color: chromeStyle.paletteRevision < 0 ? "transparent"
             : primary.pressed || root.isOpen ? chromeStyle.colors.controlPressedFill
             : hover.hovered || root.activeFocus ? chromeStyle.colors.tabHoverFill
             : chromeStyle.colors.chipFill
    }

    // What a click does, with the chord that does the same: the window's switcher opens centred from the key.
    KitToolTipHost {
        text: { root._revision; return root.controller.chipToolTip(root.fit) }
        shortcutCommand: "ShowSwitcher"
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
            objectName: "workspaceChipOpen"
            visible: root.bracketed
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            text: chromeStyle.chipOpenGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.chipText
        }

        // The status dot: a vector circle on the native rows, the state's glyph on the Terminal row.
        Rectangle {
            id: dot
            objectName: "workspaceChipDot"
            visible: root.dotShown && !root.bracketed
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            width: chromeStyle.size.hostDot
            height: chromeStyle.size.hostDot
            radius: chromeStyle.size.hostDot
            color: root.dotInk
        }

        Text {
            objectName: "workspaceChipDotGlyph"
            visible: root.dotShown && root.bracketed
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            textFormat: Text.PlainText
            text: root.controller.chipStateGlyph
            font: chromeStyle.type.footnote
            color: root.dotInk
        }

        // The daemon segment, absent for an in-process window and once the room is short.
        Text {
            objectName: "workspaceChipDaemon"
            visible: root.daemonText !== ""
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            leftPadding: root.dotShown ? root.gap : 0
            textFormat: Text.PlainText
            text: root.daemonText
            font: chromeStyle.type.footnote
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.chipText
        }

        // The state's words, while the whole chip fits.
        Text {
            objectName: "workspaceChipStatus"
            visible: root.statusText !== ""
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            leftPadding: root.gap
            textFormat: Text.PlainText
            text: root.statusText
            font: chromeStyle.type.footnote
            color: root.dotInk
        }

        Text {
            objectName: "workspaceChipSeparator"
            visible: root.daemonText !== ""
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            leftPadding: root.gap
            textFormat: Text.PlainText
            text: chromeStyle.breadcrumbGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.chipText
        }

        Text {
            objectName: "workspaceChipLabel"
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            // The room after whatever leads: the dot or the daemon segment.
            leftPadding: root.dotShown || root.daemonText !== "" ? root.gap : 0
            textFormat: Text.PlainText
            text: root.workspaceText
            font: chromeStyle.type.footnoteStrong
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.chipTextStrong
        }

        // The pop-up button's caret, a step after the name, the same one that parts the chip's other segments (a
        // cell on the Terminal row).
        Text {
            id: caret
            objectName: "workspaceChipCaret"
            anchors.verticalCenter: parent.verticalCenter
            leftPadding: root.gap
            Accessible.ignored: true
            textFormat: Text.PlainText
            text: chromeStyle.menuGlyph
            font: chromeStyle.type.footnoteStrong
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.chipTextStrong
        }

        // The marker: another workspace's status glyph, after a gap.
        Item {
            id: markerSlot
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
            objectName: "workspaceChipClose"
            visible: root.bracketed
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true
            text: chromeStyle.chipCloseGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.chipText
        }
    }
}
