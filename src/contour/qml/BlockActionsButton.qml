// vim:syntax=qml
// The ⋯ button beside the command block under the pointer: the pointer's way to the block's menu (Copy
// Output, Copy Command, Rerun, ...), which the keyboard reaches with Ctrl+Enter or Shift+F10.
//
// Where it goes is the session's blockActionsAnchor: the strip the rail lives in, beside the block's first
// visible row. The button is a square of `size.blockActions`, centred on the strip, its top at the anchor's
// top and never past the strip's end, so it covers nothing of the grid. On the Terminal row there is no strip:
// the anchor is the one cell of the block column, and the button is that cell, its glyph fitted into it.
//
// It shows and hides at once, with the anchor: there is no fade, so nothing here has to stop under Reduce
// Motion. It takes the press for itself and leaves the pointer's hover to the grid beneath, which is what
// keeps the block hovered, and the button shown, while the pointer is on it.
//
// While the block it sits beside is the block the keyboard has focused, the button is ringed as well: the
// rail's colour alone says that too, and a colour is not the only thing that should.
import QtQuick
import QtQuick.Window

Item {
    id: root
    objectName: "blockActionsButton"

    /// The TerminalSession of the pane; null while a split rebinds.
    required property var session

    /// Where the button goes, in the grid's coordinates; empty while no block is hovered.
    readonly property rect anchorRect: root.session && root.session.blockActionsAnchor !== undefined
                                       ? root.session.blockActionsAnchor : Qt.rect(0, 0, 0, 0)
    readonly property bool anchored: root.anchorRect.width > 0 && root.anchorRect.height > 0
    /// Whether the keyboard has focused the very block this button sits beside.
    readonly property bool blockFocused: !!root.session && root.session.focusedCommandBlock !== undefined
                                         && root.session.focusedCommandBlock !== 0
                                         && root.session.focusedCommandBlock === root.session.hoveredCommandBlock

    visible: root.anchored
    width: chromeStyle.size.blockActions
    // A square on the native rows; the Terminal row's cell is taller than it is wide, and the button is the cell.
    height: chromeStyle.layout.isBlockRail ? root.width : root.anchorRect.height
    // Centred on the strip, and not past its end.
    x: Math.min(root.anchorRect.x + (root.anchorRect.width - root.width) / 2,
                root.anchorRect.x + root.anchorRect.width - root.width)
    y: root.anchorRect.y

    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Block actions")
    Accessible.onPressAction: if (root.session) root.session.openBlockActions()

    Rectangle {
        id: face
        objectName: "blockActionsFace"
        anchors.fill: parent
        radius: chromeStyle.radii.blockActions
        color: chromeStyle.colors.paletteSurface
        border.width: chromeStyle.layout.isBlockRail ? chromeStyle.hairline(Screen.devicePixelRatio) : 0
        border.color: chromeStyle.colors.paletteSeparator
    }

    Text {
        objectName: "blockActionsGlyph"
        // Centred exactly, by binding rather than anchors.centerIn: that rounds onto whole pixels, so the glyph
        // sat up to half a pixel nearer one side whenever the room beside it was odd -- on the Terminal row,
        // half a hairline from the pane's edge.
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        text: "⋯"
        // The glyph keeps a hairline of room to each side of the button: on the Terminal row the button is
        // one cell and the glyph fills it, so its first dot sat on the pane's edge and read as clipped
        // there. Shrunk into the room the button leaves, on every row alike.
        readonly property real room: parent.width - 2 * chromeStyle.hairline(Screen.devicePixelRatio)
        scale: implicitWidth > room ? room / implicitWidth : 1
        font: chromeStyle.type.body
        color: chromeStyle.colors.paletteText
        Accessible.ignored: true
    }

    FocusRing {
        objectName: "blockActionsFocusRing"
        shown: root.blockFocused
        hostRadius: chromeStyle.radii.blockActions
        ground: chromeStyle.colors.paletteSurface
    }

    // The press is the button's; the hover is not, so the grid below still sees the pointer in the strip.
    TapHandler {
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: if (root.session) root.session.openBlockActions()
    }

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }
}
