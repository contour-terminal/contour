// vim:syntax=qml
// One tab in the title-bar tab strip. A colored tab is filled with its color (WT-style, faded when
// inactive/unfocused); an uncolored tab uses the OS highlight/transparent look. Single-click
// activates, double-click renames, right-click opens the context menu. Mutations go through
// `controller` (TerminalSessionManager, exposed as `terminalSessions`).
import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: root

    required property var controller
    required property var window          // ApplicationWindow, for restoring terminal focus after rename
    required property int tabIndex
    required property string tabTitle    // resolved/expanded label shown on the tab
    required property string tabRawTitle // un-expanded rename template, pre-fills the inline editor
    required property color tabColor
    required property bool tabActive
    required property int tabPaneCount
    required property bool tabZoomed     // active pane is zoomed: only it is on screen (see vtmux::Tab)

    implicitWidth: Math.min(240, Math.max(120,
        label.implicitWidth + 56 + zoomBadge.width + zoomBadge.anchors.rightMargin))
    implicitHeight: 32

    // Live OS palette handle so the tab adapts to dark/light in realtime (see TabContextMenu).
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Uncolored tabs report a transparent accentColor (WindowController ColorRole), so alpha is the flag.
    readonly property bool colored: root.tabColor.a > 0

    // Subtle highlight wash shared by the uncolored-tab hover fill and the close button's hover, so the
    // hover tint (color + alpha) is defined once.
    readonly property color hoverWash: Qt.rgba(systemPalette.highlight.r, systemPalette.highlight.g,
                                               systemPalette.highlight.b, 0.25)

    // The fill behind the label; also drives text contrast. Colored: the WT-style fade from
    // TabColorScheme.h (via the controller). Uncolored: OS highlight / hover wash / transparent.
    // controller is null-guarded (torn down before this tree on window close).
    readonly property color effectiveBackground: {
        if (root.colored && root.controller)
            return root.controller.tabBackgroundColor(root.tabColor, systemPalette.window,
                                                      root.tabActive, hover.hovered, Window.active)
        if (root.tabActive)
            return systemPalette.highlight
        if (hover.hovered)
            return root.hoverWash
        return "transparent"
    }

    // Label / close-glyph color: contrast against a colored fill, else the matching palette role.
    readonly property color foreground: {
        if (root.colored && root.controller)
            return root.controller.tabTextColor(root.effectiveBackground)
        return root.tabActive ? systemPalette.highlightedText : systemPalette.windowText
    }

    // The pixmap the OS drag cursor shows while this tab is dragged (see dragProxy). grabToImage is
    // ASYNCHRONOUS — it delivers the url on a later frame — so we pre-grab here and re-grab whenever the
    // tab's size/label/color/active-state changes, keeping a ready image for every drag. If it is still
    // empty on a first-ever drag the drag still works (Qt uses a default pixmap); only the ghost is missing.
    property url dragImage: ""
    function refreshDragImage() {
        // grabToImage warns (and, offscreen, fails the QML test gate) unless the item is realized: it
        // needs a non-zero size and a visible window. Skip until then; the tab re-grabs when it lands on
        // a visible window (onVisibleChanged) or its size/label/color change.
        if (root.width <= 0 || root.height <= 0)
            return
        if (!root.Window.window || !root.Window.window.visible)
            return
        root.grabToImage(function(result) { root.dragImage = result.url })
    }
    Component.onCompleted: root.refreshDragImage()
    onWidthChanged: root.refreshDragImage()
    onVisibleChanged: root.refreshDragImage()
    onTabTitleChanged: root.refreshDragImage()
    onTabColorChanged: root.refreshDragImage()
    onTabActiveChanged: root.refreshDragImage()
    Connections {
        target: root.Window.window
        function onVisibleChanged() { root.refreshDragImage() }
    }

    Rectangle {
        anchors.fill: parent
        color: root.effectiveBackground
    }

    HoverHandler {
        id: hover
        // Built when the hover STARTS, not bound: the working directory changes with every `cd`, and a
        // binding would have to be driven by a change signal the model does not emit. A tooltip is only
        // read while it is open, so composing it on open is both simpler and always current.
        onHoveredChanged: if (hovered) root.hoverTooltip = root.buildHoverTooltip()
    }

    // What the hover tooltip shows. Empty means "nothing worth saying", which suppresses it entirely.
    property string hoverTooltip: ""

    /// The full title (only when the tab is too narrow to show it) above the session's working directory.
    ///
    /// The directory is the point: with several tabs running full-screen applications, nothing on screen
    /// says where any of them was started, and the tab label is usually the application's name.
    function buildHoverTooltip() {
        var lines = []
        // A title the tab already shows in full adds nothing; repeating it is just noise.
        if (label.truncated)
            lines.push(root.tabTitle)
        var cwd = root.controller ? root.controller.tabWorkingDirectory(root.tabIndex) : ""
        if (cwd !== "")
            lines.push(cwd)
        return lines.join("\n")
    }

    ToolTip.text: root.hoverTooltip
    // Suppressed while renaming: the editor covers the label, and the tooltip would sit over the field
    // the user is typing into.
    ToolTip.visible: hover.hovered && root.hoverTooltip !== "" && !renameLoader.active
    ToolTip.delay: 600

    // --- Tab drag: reorder within the strip, move to another window, or tear out to a new window. ---
    // Uses an AUTOMATIC drag (Drag.dragType: Drag.Automatic): when Drag.active goes true it runs a real
    // QDrag::exec() that — unlike the default Internal drag, which is confined to one window's scene —
    // crosses top-level windows in this process, so a DropArea in ANOTHER window receives it (that is what
    // makes both cross-window drop AND the target strip's drop-caret work). The payload rides in
    // Drag.mimeData because drop.source is not reliable across an OS QDrag; the destination reads it via
    // drop.getDataAsString(). The real tab stays put — the OS drag cursor shows dragImage as the ghost.
    Item {
        id: dragProxy
        Drag.dragType: Drag.Automatic
        Drag.active: dragHandler.active
        Drag.hotSpot.x: width / 2
        Drag.hotSpot.y: height / 2
        Drag.supportedActions: Qt.MoveAction
        Drag.proposedAction: Qt.MoveAction
        Drag.imageSource: root.dragImage
        // windowId is a small sequential integer (SessionModel _nextWindowId++), so a plain JS number
        // carries it without truncation; a DropArea in any window reads this back via getDataAsString().
        Drag.mimeData: ({
            "application/x-contour-tab": JSON.stringify({
                windowId: root.controller ? root.controller.windowIdValue() : 0,
                tabIndex: root.tabIndex
            })
        })
        // The automatic QDrag has fully returned here with the real OS drop action. A strip DropArea that
        // accepted the tab called drop.accept(Qt.MoveAction); dropping on empty desktop yields IgnoreAction
        // -> tear the tab out into its own window. (This replaces the old Internal-drag Drag.drop() path.)
        Drag.onDragFinished: (dropAction) => {
            if (dropAction === Qt.IgnoreAction && root.controller)
                root.controller.tearOffTab(root.tabIndex)
        }
    }

    DragHandler {
        id: dragHandler
        target: dragProxy
        // A small threshold so a click/tap still activates or renames; only a real drag begins a move.
        // Activating this handler arms dragProxy.Drag.active, which starts the automatic QDrag; the
        // move/tear-off decision is made by the DropArea and dragProxy.Drag.onDragFinished, not here.
        dragThreshold: 8
    }

    Text {
        id: label
        objectName: "tabLabel"   // findChild() handle for the GUI test
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.right: zoomBadge.left
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        text: root.tabTitle
        color: root.foreground
        elide: Text.ElideRight
        visible: !renameLoader.active
    }

    // Zoom badge. A zoomed tab shows only one of its panes, which makes it look exactly like a
    // genuinely single-pane tab — this is the cue that the siblings are hidden rather than gone.
    // tmux marks the same state with a "Z", so the glyph should read as familiar rather than novel.
    Rectangle {
        id: zoomBadge
        objectName: "zoomBadge"   // findChild() handle for the GUI test
        anchors.right: closeButton.left
        // Collapse to nothing — width AND margin — when not zoomed. The label anchors to this item's
        // left edge, so anything it still occupies is stolen from every tab's title, zoomed or not:
        // a fixed margin here would elide the title 4px early on tabs that have no badge at all.
        anchors.rightMargin: root.tabZoomed ? 4 : 0
        anchors.verticalCenter: parent.verticalCenter
        width: root.tabZoomed ? height : 0
        height: 16
        radius: 3
        visible: root.tabZoomed
        // Tinted from the tab's own foreground, so the badge tracks the active/inactive and
        // colored/uncolored variants instead of hardcoding a color per theme.
        color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.18)

        Text {
            anchors.centerIn: parent
            text: "Z"
            font.pixelSize: 10
            font.bold: true
            color: root.foreground
        }
    }

    // Inline rename field, shown on double-click.
    Loader {
        id: renameLoader
        active: false
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.right: zoomBadge.left
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        function start() { active = true }
        sourceComponent: TextField {
            // Edit the RAW template (e.g. "a: {WindowTitle}"), not the expanded label the tab shows.
            // Empty for a never-renamed tab, so the editor opens blank.
            text: root.tabRawTitle
            selectByMouse: true
            Component.onCompleted: { forceActiveFocus(); selectAll() }
            // Guards the focus-out handler from re-finalizing an edit already finalized by Enter or
            // Escape (deactivating the Loader also drops focus, re-entering onActiveFocusChanged).
            property bool finalized: false
            // On accept the rename completes deterministically, so hand keyboard focus back to the
            // terminal (the field grabbed it on open). Escape cancels. The focus-out path does NOT
            // restore focus because focus has already moved to wherever the user clicked.
            onAccepted: {
                finalized = true
                root.controller.setTabTitle(root.tabIndex, text)
                renameLoader.active = false
                root.window.restoreTerminalFocus()
            }
            Keys.onEscapePressed: {
                finalized = true // cancel: discard the edit
                renameLoader.active = false
                root.window.restoreTerminalFocus()
            }
            // Clicking the terminal / another tab while editing COMMITS the typed name (matching the
            // common editor convention) rather than silently discarding it. Enter/Escape already
            // finalized (and set `finalized`), so this only runs for a genuine click-away. Do not
            // restore terminal focus here: focus has already moved to the user's click target, and
            // forcing it back would fight that.
            onActiveFocusChanged: {
                if (!activeFocus && !finalized) {
                    finalized = true
                    // Focus is also lost when the window tears down with the editor open; the
                    // controller is already null then, and there is no rename left to commit.
                    if (root.controller)
                        root.controller.setTabTitle(root.tabIndex, text)
                    renameLoader.active = false
                }
            }
        }
    }

    ToolButton {
        id: closeButton
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: 22
        height: 22
        text: "✕" // ✕
        font.pointSize: 8
        // Don't take keyboard focus away from the terminal when clicked.
        focusPolicy: Qt.NoFocus
        onClicked: root.controller.closeTabAtIndex(root.tabIndex)
        // Flat, transparent chrome so the button blends into the tab fill instead of showing an opaque
        // style button panel; a subtle highlight wash gives hover feedback.
        background: Rectangle {
            color: closeButton.hovered ? root.hoverWash : "transparent"
        }
        // Share the label's foreground so the glyph stays legible on colored fills and in light mode.
        contentItem: Text {
            text: closeButton.text
            font: closeButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: root.foreground
        }
    }

    // A single left-tap activates the tab; a double-tap starts an inline rename. A TapHandler fires
    // onTapped for EVERY tap (tapCount is the running consecutive-tap count), so a double-tap fires
    // first with tapCount===1 (activate) then tapCount===2 (rename).
    //
    // Activate on the FIRST tap rather than deferring it by the double-click interval: activating an
    // already-active tab is a harmless no-op (SessionModel::activateTab early-outs), so the second tap of
    // a rename gesture re-activates the (already-active) tab and then opens the editor — you always rename
    // the tab you clicked. Deferring instead cost every single click the full double-click-interval latency
    // and, on platforms/styles where that interval is reported as 0, fired the deferred activation
    // synchronously so a genuine double-tap flashed the wrong tab before renaming.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: (eventPoint, button) => {
            root.controller.activateTab(root.tabIndex)
            if (tapCount === 2)
                renameLoader.start()
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: contextMenu.popup()
    }

    TabContextMenu {
        id: contextMenu
        controller: root.controller
        tabIndex: root.tabIndex
        onRenameRequested: renameLoader.start()
        // The menu only raises this once it has closed, so the mouse path and the keyboard path below
        // are the same one-liner: ask this tab's flyout to open.
        onColorRequested: colorFlyout.open()
    }

    // The swatch-grid color picker (WT-style quick coloring). It lives HERE rather than inside the
    // context menu because it has two entry points that must share one instance: "Choose Color…" in
    // the menu, and the SetTabColor action below (which fires with no menu open at all).
    //
    // A Popup positions itself against its PARENT, which is now this tab rather than the menu that used
    // to own it. The flyout places itself against that parent (below the tab, or above it when the tab
    // strip sits at the bottom of the window) — see TabColorFlyout's own placement block.
    TabColorFlyout {
        id: colorFlyout
        objectName: "tabColorFlyout"
        controller: root.controller
        tabIndex: root.tabIndex
        // Which swatch the keyboard cursor starts on when the flyout opens.
        currentColor: root.tabColor
    }

    // Keyboard entry points (the SetTabTitle / SetTabColor actions): the WindowController emits
    // tabTitleEditRequested(index) / tabColorPickRequested(index) for the active tab; the matching
    // delegate opens its editor or its flyout. The active tab's delegate is always realized, so an
    // in-delegate Connections reliably fires.
    Connections {
        target: root.controller
        function onTabTitleEditRequested(index) {
            if (index === root.tabIndex)
                renameLoader.start()
        }
        function onTabColorPickRequested(index) {
            if (index === root.tabIndex)
                colorFlyout.open()
        }
    }
}
