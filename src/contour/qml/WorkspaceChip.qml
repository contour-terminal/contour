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
// says, the tooltip, the accessible words -- is the chip model's (WindowController.chip, window/LocationChipModel.hpp).
// The chip measures its steps (`fitWidths`) and lays out whichever `fit` the title bar sets from the room it computes. Rest,
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
    /// The window's controller, which owns the chip's model (`chip`).
    required property var controller
    /// How much of the chip fits, a window::LocationChipFit as its underlying value: the title bar's to set,
    /// from `fitWidths` and the room it leaves (never from the chip's own width, which would loop).
    property int fit: 0
    /// Whether the chip names the daemon alone (`local ▾`, the control that heads the sidebar) rather than the
    /// daemon and the workspace: the same chip, the workspace segment left out.
    property bool daemonOnly: false
    /// Whether keyboard focus is on the chip and the chrome says so: the host's to set.
    property bool focusRingShown: false
    /// Whether the dropdown the chip opened is open: the chip wears its pressed fill meanwhile.
    property bool isOpen: false
    // Whether the dropdown was open as the press began: that press closed it, and its release must not open
    // it again (ui-ux rule 2).
    property bool _wasOpenAtPress: false

    /// The chip's model: everything the chip says, and the texts at every fit step.
    readonly property var chip: root.controller.chip
    /// The fit steps in elision order, each { fit, key, daemon, status, workspace, tooltip }: the model's to say,
    /// so the order and the keys have one source. A step is at the position its `fit` names.
    readonly property var fitSteps: root.chip.steps
    /// The step the current `fit` names.
    readonly property var step: root.fitSteps[root.fit]
    readonly property bool marked: !!root.sidebar && root.sidebar.workspaceChipMarked
    readonly property bool bracketed: chromeStyle.chipOpenGlyph !== ""
    readonly property real gap: root.bracketed ? chromeStyle.cellWidth : chromeStyle.space.s3
    /// Whether the status dot (or, bracketed, its state glyph) shows: the model's call.
    readonly property bool dotShown: root.chip.dotShown
    // How the connection looks: the dot's ink follows the palette.
    readonly property var look: chromeStyle.paletteRevision < 0
                                ? ({}) : chromeStyle.hostChipLook(root.chip.connectionState)
    readonly property color dotInk: root.look.dot !== undefined ? root.look.dot : "transparent"
    /// What the daemon segment, the state and the workspace say at the current fit.
    readonly property string daemonText: root.step.daemon
    readonly property string statusText: root.step.status
    readonly property string workspaceText: root.step.workspace
    /// What each fit step needs, by the steps' keys, in pixels (whole cells on the Terminal row). Independent
    /// of `fit`, so the title bar can choose `fit` from it.
    readonly property var fitWidths: {
        fontMetrics.font
        strongMetrics.font
        const widths = {}
        root.fitSteps.forEach(step => { widths[step.key] = root._widthAt(step) })
        return widths
    }

    /// What the chip needs at its last fit step, the least it gets: the title bar reserves it, so the window is
    /// never made narrower than that. Counted with the state dot and the attention marker present, whether or
    /// not they are, so the window's minimum does not move with the connection or with another workspace.
    readonly property real leastWidth: {
        fontMetrics.font
        strongMetrics.font
        return root._widthAt(root.fitSteps[root.fitSteps.length - 1], true)
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

    // The state glyph's width on the Terminal row: the current one's, or with `reserved` the widest of them.
    function _dotSpan(reserved) {
        if (!reserved)
            return root._span(fontMetrics, root.chip.stateGlyph)
        return Math.max(...root.chip.stateGlyphs.map(glyph => root._span(fontMetrics, glyph)))
    }

    // What the chip needs at one fit step: what stays whatever fits (the ends, the caret and the marker) and
    // the dot, the daemon, the state and the workspace as that step says them. `reserved` counts the dot (of a
    // chip that names a daemon) and the marker as shown, whether or not they are.
    function _widthAt(step, reserved) {
        const daemon = step.daemon
        const status = step.status
        const workspace = step.workspace
        let width = root.bracketed ? root._span(fontMetrics, chromeStyle.chipOpenGlyph + chromeStyle.chipCloseGlyph)
                                   : 2 * chromeStyle.space.s5
        width += caret.leftPadding + root._span(strongMetrics, chromeStyle.menuGlyph)
        if (reserved)
            width += root.gap + (root.bracketed ? chromeStyle.badgeHeight : chromeStyle.size.badge)
        else
            width += markerSlot.visible ? markerSlot.width : 0
        let leading = false
        if (root.dotShown || (reserved && root.chip.daemonName !== "")) {
            width += root.bracketed ? root._dotSpan(reserved) : chromeStyle.size.hostDot
            leading = true
        }
        if (daemon !== "") {
            width += (leading ? root.gap : 0) + root._span(fontMetrics, daemon)
            leading = true
        }
        if (status !== "")
            width += root.gap + root._span(fontMetrics, status)
        if (root.daemonOnly)
            return width
        if (daemon !== "")
            width += root.gap + root._span(fontMetrics, chromeStyle.breadcrumbGlyph)
        return width + (leading ? root.gap : 0) + root._span(strongMetrics, workspace)
    }

    Accessible.role: Accessible.ButtonMenu
    Accessible.focusable: root.focusRingShown
    Accessible.focused: root.focusRingShown
    // What it shows: the workspace and its daemon, or the daemon alone where it heads the sidebar.
    Accessible.name: root.daemonOnly ? root.chip.daemonAccessibleName : root.chip.accessibleName
    Accessible.description: root.chip.accessibleDescription
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
        text: root.step.tooltip
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
            text: root.chip.stateGlyph
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
            visible: root.daemonText !== "" && !root.daemonOnly
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
            visible: !root.daemonOnly
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
