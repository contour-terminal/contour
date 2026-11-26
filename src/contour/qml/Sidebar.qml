// vim:syntax=qml
// The workspace sidebar: every workspace the window can show, their tabs and the panes of the split ones, as
// one tree (WindowController.sidebar, a SidebarModel), with a title row (its toggle, its heading, its Needs-me
// and filter buttons and, where the row keeps it there, its new-workspace button), a type-to-filter field, the
// Needs-me scope's empty state, the footer's counts and a divider the user drags to resize it.
//
// The model decides what every row says and the C++ side every rule: the order statuses rank in, what the
// keys of the focused tree mean (WindowController.chromeKey, which hands the tree's own commands back through
// sidebarCommandRequested), which row the Collapse and Expand keys land on, the bounds a width is kept in
// (WindowController.setSidebarWidth). This file only lays the parts out and carries the commands out. One
// component tree for the four chrome rows: the source list (macOS, beside the band, its top under the
// traffic lights), the navigation pane (Fluent), the header pane (ContourLinux, whose title row is the header
// bar's part above it, TitleBar.qml) and the cell tree (the Terminal row, divided from the content by a
// one-cell column of `│` that never moves).
import QtQuick
import QtQuick.Controls
import Contour.Terminal

Rectangle {
    id: root
    objectName: "sidebar"

    /// The window's controller (Main.qml: appWindow.win).
    required property var controller
    /// The window, for the terminal's focus.
    required property var window
    /// The height of the band the title row stands beside, when the sidebar is the window's full height
    /// (chromeStyle.sidebar.besideBand); its title row is that tall, so the OS's controls fit over it.
    property real bandHeight: 0
    /// What the OS keeps at the window's leading edge for its own window controls, which sit over the title
    /// row of a sidebar beside the band (WindowController.nativeControlsInset).
    property real controlsInset: 0
    /// Whether the header bar above draws the title row (UiStyleProvider.sidebarTitleInHeaderBar): then the
    /// sidebar draws none of its own.
    property bool titleInHeaderBar: false
    /// Where the location chip shows (WindowController.chip.place, Main.qml's to pass): "sidebar" puts a tmux
    /// window's host control at the head of the title row, where the heading would be.
    property string chipPlace: "none"
    /// The sidebar's toggle as UiStyleProvider::sidebarToggle answers it (Main.qml): `host` "sidebar" puts it
    /// at the title row's leading end, at the spot the band holds it while the sidebar is hidden, and `label`
    /// is what it says.
    property var sidebarToggle: ({})

    readonly property var model: root.controller ? root.controller.sidebar : null
    readonly property var commands: root.controller && root.controller.sidebarCommands
                                    ? root.controller.sidebarCommands : ({})
    /// Whether the keyboard is in the tree, so its current row wears the ring.
    readonly property bool treeFocused: !!root.controller && !!root.controller.sidebarHasFocus && tree.activeFocus
    /// The width while the divider is dragged; the controller's width otherwise.
    property real liveWidth: -1
    /// The width the sidebar takes: while it is dragged, where the drag is; else the controller's.
    // The controller counts whole pixels; the width is the row's own units, whole cells on the Terminal row.
    readonly property real shownWidth: root.liveWidth >= 0 ? root.liveWidth
        : ((root.controller && root.controller.sidebarWidth) || 0) > 0
          ? chromeStyle.snappedSidebarWidth(root.controller.sidebarWidth) : 0
    /// The dividing column's width: one cell on the cell tree, a hairline elsewhere.
    readonly property real dividerWidth: chromeStyle.sidebar.dividerGlyph !== "" ? chromeStyle.cellWidth
                                                                                : chromeStyle.borderWidth
    /// The title row's toolbar items on screen, in visual order: a tmux window's host chip heading it, the
    /// switcher and the Needs-me control. The chrome's keyboard walk (Ctrl+F6) visits them (Main.qml).
    readonly property var toolbarControls: [daemonChipSlot.item, switcherButton, needsMeButton]
        .filter((item) => item !== null && item.visible && titleRow.visible)
    /// Whether the host control heads the title row, in the heading's place: the one answer the heading's
    /// visibility and the control's loading both follow. Only a host that is no daemon (a tmux attach) puts
    /// it here; a daemon's window names its daemon in the tree's heading row.
    readonly property bool daemonChipShown: root.chipPlace === "sidebar" && !!root.controller && titleRow.visible
    /// The tree (a ListView), for the tests.
    readonly property alias tree: tree
    /// Whether the user opened the filter field (a typed key, the filter button or its action).
    property bool filterOpen: false
    /// Whether the Needs-me toggle stands beside the filter field (chromeStyle.sidebar.needsMeInFilterRow):
    /// "Needs me" is a scope of the filter, and that look's title row has no room for a fourth button.
    readonly property bool needsMeInFilterRow: chromeStyle.sidebar.needsMeInFilterRow
    /// Whether the Needs-me scope narrows the tree (a config::SidebarScope other than All).
    readonly property bool narrowed: !!root.model && root.model.scope !== SidebarRows.SidebarScope.All

    color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarFill
    clip: true

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Workspace sidebar")

    /// Puts the keyboard on the tree, at the row of the tab the window shows unless it is on a row already.
    function focusTree() {
        if (tree.currentIndex < 0 || tree.currentIndex >= tree.count)
            tree.currentIndex = root.model && root.model.currentIndex >= 0 ? root.model.currentIndex : 0
        tree.forceActiveFocus()
    }

    /// Carries out a command of the focused tree (WindowController.sidebarCommandRequested).
    function runCommand(command, text) {
        const current = tree.currentIndex
        switch (command) {
        case root.commands.previous: tree.decrementCurrentIndex(); break
        case root.commands.next: tree.incrementCurrentIndex(); break
        case root.commands.first: tree.currentIndex = 0; break
        case root.commands.last: tree.currentIndex = tree.count - 1; break
        case root.commands.activate: root.controller.activateSidebarRow(current); break
        case root.commands.collapse: tree.currentIndex = root.model.collapseOrParent(current); break
        case root.commands.expand: tree.currentIndex = root.model.expandOrChild(current); break
        case root.commands.rename: root.renameRow(current); break
        case root.commands.filter:
            root.filterOpen = true
            filterField.text += text
            filterField.forceActiveFocus()
            break
        case root.commands.clearFilter:
            filterField.text = ""
            root.filterOpen = false
            break
        case root.commands.openMenu: root.openRowMenu(current); break
        case root.commands.openInNewWindow: if (root.model) root.model.openInNewWindow(current); break
        }
        tree.positionViewAtIndex(tree.currentIndex, ListView.Contain)
    }

    /// What Return (with @p modifiers) in the filter field does: activates the first row it left.
    function acceptFilter(modifiers) {
        const mode = (modifiers & Qt.ShiftModifier) ? SidebarRows.JumpMode.NewWindow : SidebarRows.JumpMode.Here
        // With nothing typed and every row listed there is no match: the tree takes the keyboard, as Down does.
        if (root.controller && root.controller.activateSidebarFirstMatch
                && root.controller.activateSidebarFirstMatch(mode) === SidebarRows.RowActivation.Unfiltered)
            tree.forceActiveFocus()
    }

    /// Opens the filter field and puts the keyboard in it: the filter button, and the header bar's.
    function openFilter() {
        root.filterOpen = true
        filterField.forceActiveFocus()
    }

    /// Opens row @p index's context menu below it, as a right click on it does: the Menu key and Shift+F10.
    function openRowMenu(index) {
        if (!root.model || index < 0 || index >= tree.count)
            return
        tree.positionViewAtIndex(index, ListView.Contain)
        const item = tree.itemAtIndex(index)
        if (item)
            root.model.openContextMenu(index, item.mapToItem(null, chromeStyle.space.s5, item.height))
    }

    /// Opens the inline editor on row @p index, when it is a workspace's or a tab's (a pane's has none).
    function renameRow(index) {
        if (index < 0 || index >= tree.count)
            return
        tree.currentIndex = index
        // A sidebar shown for the rename has laid out no rows yet.
        tree.forceLayout()
        tree.positionViewAtIndex(index, ListView.Contain)
        const item = tree.itemAtIndex(index)
        if (item)
            item.startRename()
    }

    /// Opens the colour picker over workspace @p workspace's row (the controller's workspaceColorPickRequested):
    /// "Custom…" in its menu's Workspace Color, or the SetWorkspaceColor action naming no colour.
    function openWorkspaceColorPicker(workspace) {
        if (!root.model)
            return
        const item = root.colorPickerRow(root.model.workspaceRowIndex(workspace))
        if (!item)
            return
        workspaceColorFlyout.workspace = workspace
        root.openColorPicker(workspaceColorFlyout, item, item.workspaceColor)
    }

    /// Opens the colour picker over tab row @p index for tab @p tab (the controller's
    /// sidebarTabColorPickRequested): "Custom…" in a tab row's menu's Tab Color.
    function openTabColorPicker(index, tab) {
        const item = root.colorPickerRow(index)
        if (!item)
            return
        tabColorFlyout.tab = tab
        root.openColorPicker(tabColorFlyout, item, item.tabColor)
    }

    /// The laid-out row @p index a colour picker hangs from; null when there is none.
    function colorPickerRow(index) {
        if (index < 0 || index >= tree.count)
            return null
        // A sidebar shown for the picker has laid out no rows yet.
        tree.forceLayout()
        tree.positionViewAtIndex(index, ListView.Contain)
        return tree.itemAtIndex(index)
    }

    /// Opens @p flyout hung from row @p item, its keyboard cursor on @p color.
    function openColorPicker(flyout, item, color) {
        flyout.currentColor = color
        // It hangs from the row, as the tab strip's hangs from its tab (TabColorFlyout's placement).
        flyout.parent = item
        flyout.open()
    }

    /// The editor or the filter let go of the keyboard: the tree takes it while the sidebar holds the chrome
    /// focus, the terminal otherwise.
    function returnFocus() {
        if (root.controller && root.controller.sidebarHasFocus)
            tree.forceActiveFocus()
        else if (root.window && root.window.restoreTerminalFocus)
            root.window.restoreTerminalFocus()
    }

    // After a jump or a switch the selected row may be anywhere: it is brought into view once it is another
    // row (SidebarModel::currentRowChanged), not each time a growing section above it shifts its index, which
    // would snap back a view the user scrolls.
    Connections {
        target: root.model
        function onCurrentRowChanged() {
            if (root.model.currentIndex >= 0)
                tree.positionViewAtIndex(root.model.currentIndex, ListView.Contain)
        }
    }

    Connections {
        target: root.controller
        ignoreUnknownSignals: true // a lightweight mock controller may carry no sidebar commands
        function onSidebarCommandRequested(command, text) { root.runCommand(command, text) }
        function onWorkspaceRenameRequested() {
            if (root.model)
                root.renameRow(root.model.currentWorkspaceIndex)
        }
        function onSidebarRowRenameRequested(index) { root.renameRow(index) }
        function onSidebarFilterRequested() { root.openFilter() }
        function onWorkspaceColorPickRequested(workspace) { root.openWorkspaceColorPicker(workspace) }
        function onSidebarTabColorPickRequested(index, tab) { root.openTabColorPicker(index, tab) }
    }

    // The one colour picker of the sidebar's workspaces, hung from the row it colours when it opens. A pick
    // colours that workspace by id, shown or not, and leaves the window where it is.
    TabColorFlyout {
        id: workspaceColorFlyout
        objectName: "sidebarWorkspaceColorFlyout"
        /// The workspace the picker colours, by its id (WindowController::workspaceColorPickRequested).
        property var workspace: 0
        controller: root.controller
        // Null-guarded: the controller is torn down before this popup on window close.
        onColorPicked: (c) => {
            if (root.controller)
                root.controller.setWorkspaceColor(workspaceColorFlyout.workspace, c)
        }
        onResetRequested: {
            if (root.controller)
                root.controller.resetWorkspaceColor(workspaceColorFlyout.workspace)
        }
        onClosed: root.returnFocus()
    }

    // The one colour picker of the sidebar's tabs, as the workspaces' above: a sidebar placement draws no tab
    // strip to hang the tab's own from. A pick colours that tab by id and leaves the focus where it is.
    TabColorFlyout {
        id: tabColorFlyout
        objectName: "sidebarTabColorFlyout"
        /// The tab the picker colours, by its id (WindowController::sidebarTabColorPickRequested).
        property var tab: 0
        controller: root.controller
        // Null-guarded: the controller is torn down before this popup on window close.
        onColorPicked: (c) => {
            if (root.controller)
                root.controller.setTabColorOf(tabColorFlyout.tab, c)
        }
        onResetRequested: {
            if (root.controller)
                root.controller.resetTabColorOf(tabColorFlyout.tab)
        }
        onClosed: root.returnFocus()
    }

    // The title row, unless the header bar draws it above the sidebar: the sidebar's toggle where the band
    // leaves it here, the heading ("Workspaces", where the row names its list), the Needs-me and filter
    // buttons and, where the row keeps it here rather than in the footer, the "+" that makes a workspace.
    // Beside the band it is the band's height, the OS's controls over its leading end. Its empty places are
    // the title bar's: a drag moves the window, a double-click maximises it, as the band does
    // (TitleBarGestures.qml).
    Item {
        id: titleRow
        objectName: "sidebarTitleRow"
        visible: !root.titleInHeaderBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: !titleRow.visible ? 0
              : chromeStyle.sidebar.besideBand ? Math.max(root.bandHeight, chromeStyle.sidebar.rowHeight)
              : chromeStyle.sidebar.rowHeight

        // The gap at the row's ends and between its buttons: a cell on the Terminal row, where the small
        // spacing steps are no cells at all, so nothing touches the divider's column.
        readonly property real gap: chromeStyle.sidebar.dividerGlyph !== ""
                                    ? Math.max(chromeStyle.space.s3, chromeStyle.cellWidth) : chromeStyle.space.s3
        readonly property real buttonExtent: Math.min(chromeStyle.size.toolbarButton, titleRow.height)
        // Between the row's buttons: the smaller step, so four buttons, the toggle and the OS's controls share
        // the default width; a cell on the Terminal row, as at the row's ends.
        readonly property real buttonGap: chromeStyle.sidebar.dividerGlyph !== "" ? titleRow.gap : chromeStyle.space.s2

        TitleBarGestures {
            objectName: "sidebarTitleGestures"
            anchors.fill: parent
            windowController: root.controller ? root.controller : null
        }

        // At the band's toggle's spot: after the OS's controls, which the band also starts after.
        SidebarControlButton {
            id: toggleButton
            objectName: "sidebarToggle"
            control: "toggle"
            visible: root.sidebarToggle.host === "sidebar"
            anchors.left: parent.left
            anchors.leftMargin: chromeStyle.snapToCells(root.controlsInset)
            anchors.verticalCenter: parent.verticalCenter
            extent: titleRow.buttonExtent
            implicitWidth: chromeStyle.snapToCells(extent)
            accessibleName: root.sidebarToggle.label ?? ""
            checkable: true
            checked: root.sidebarToggle.shown ?? false
            onClicked: if (root.controller && root.controller.toggleSidebar) root.controller.toggleSidebar()
        }

        Text {
            objectName: "sidebarTitle"
            visible: chromeStyle.sidebar.named && !root.daemonChipShown
            anchors.left: toggleButton.visible ? toggleButton.right : parent.left
            anchors.leftMargin: toggleButton.visible ? chromeStyle.space.s3
                              : chromeStyle.snapToCells(root.controlsInset) + chromeStyle.space.s4
            anchors.right: titleButtons.left
            anchors.verticalCenter: parent.verticalCenter
            textFormat: Text.PlainText
            text: chromeStyle.sidebarHeading
            font: chromeStyle.type.footnoteStrong
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarSecondaryInk
            elide: Text.ElideRight
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }

        // A tmux window's host control, heading the row in the heading's place (or, on a row with no heading,
        // right after the toggle): the band holds no chip while the sidebar shows, so the host is named here
        // alone. A daemon's window names its daemon in the tree's heading row instead (chip.place).
        Loader {
            id: daemonChipSlot
            active: root.daemonChipShown
            anchors.left: toggleButton.visible ? toggleButton.right : parent.left
            anchors.leftMargin: toggleButton.visible ? chromeStyle.space.s3
                              : chromeStyle.snapToCells(root.controlsInset) + chromeStyle.space.s4
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: WorkspaceChip {
                objectName: "sidebarDaemonChip"
                daemonOnly: true
                sidebar: root.model
                controller: root.controller
                // The room the row has (the filter and "+" buttons stand to its right).
                available: Math.max(0, titleButtons.x - daemonChipSlot.x - titleRow.gap)
                toolbarHasFocus: (root.controller.focusedToolbarIndex ?? -1) >= 0
            }
        }

        Row {
            id: titleButtons
            // Which controls give way to a narrow sidebar (UiStyleProvider.sidebarControlFit): the Needs-me
            // control first, then the switcher. The room is the row's, less its leading toggle and inset.
            readonly property var fit: chromeStyle.sidebarControlFit(
                // Each control is counted with the gap before it; the first one's stands for the row's end margin.
                titleRow.width - root.dividerWidth - titleRow.gap + titleRow.buttonGap
                    - chromeStyle.snapToCells(root.controlsInset)
                    - (toggleButton.visible ? toggleButton.width + titleRow.gap : 0),
                chromeStyle.snapToCells(titleRow.buttonExtent) + titleRow.buttonGap,
                [switcherButton.wanted, !root.needsMeInFilterRow, true, newWorkspaceButton.visible]
                    .filter((wants) => wants).length)
            anchors.right: parent.right
            anchors.rightMargin: root.dividerWidth + titleRow.gap
            anchors.verticalCenter: parent.verticalCenter
            spacing: titleRow.buttonGap

            SidebarSwitcherButton {
                id: switcherButton
                objectName: "sidebarSwitcherButton"
                controller: root.controller
                visible: wanted && (titleButtons.fit.switcher ?? true)
                extent: titleRow.buttonExtent
                implicitWidth: chromeStyle.snapToCells(extent)
            }

            SidebarNeedsMeButton {
                id: needsMeButton
                objectName: "sidebarNeedsMeButton"
                controller: root.controller
                visible: !root.needsMeInFilterRow && (titleButtons.fit.needsMe ?? true)
                extent: titleRow.buttonExtent
                implicitWidth: chromeStyle.snapToCells(extent)
            }

            SidebarControlButton {
                objectName: "sidebarFilterButton"
                control: "filter"
                extent: titleRow.buttonExtent
                implicitWidth: chromeStyle.snapToCells(extent)
                accessibleName: qsTr("Filter workspaces")
                // The FilterWorkspaces action's way, so a click and its key binding do one thing.
                onClicked: {
                    if (root.controller && root.controller.openSidebarFilter)
                        root.controller.openSidebarFilter()
                    else
                        root.openFilter()
                }
            }

            SidebarControlButton {
                id: newWorkspaceButton
                objectName: "sidebarNewWorkspaceButton"
                control: "newWorkspace"
                visible: !chromeStyle.sidebar.addInFooter
                extent: titleRow.buttonExtent
                implicitWidth: chromeStyle.snapToCells(extent)
                accessibleName: qsTr("New workspace")
                dropSidebar: root.model
                onClicked: if (root.controller && root.controller.newWorkspace) root.controller.newWorkspace()
            }
        }
    }

    // Type-to-filter: opened by a typed key in the tree, it narrows the rows (SidebarModel.setFilter); Escape
    // clears it and gives the tree the keyboard back. Where the look keeps the Needs-me toggle beside it, the
    // row also stands while that scope narrows the tree, so the toggle that narrowed it is there to undo it.
    TextField {
        id: filterField
        objectName: "sidebarFilterField"
        visible: root.filterOpen || (root.needsMeInFilterRow && root.narrowed)
        anchors.left: parent.left
        anchors.right: filterNeedsMeButton.visible ? filterNeedsMeButton.left : parent.right
        anchors.top: titleRow.bottom
        anchors.leftMargin: chromeStyle.space.s3
        anchors.rightMargin: filterNeedsMeButton.visible ? chromeStyle.space.s2
                                                         : root.dividerWidth + chromeStyle.space.s3
        height: visible ? implicitHeight : 0
        font: chromeStyle.type.body
        Accessible.role: Accessible.EditableText
        Accessible.name: qsTr("Filter workspaces")
        onTextChanged: if (root.model) root.model.setFilter(text)
        // A jump under way is what Escape ends first, as in the tree (window::sidebarEscapeCommand()); the
        // next one clears the filter and gives the keyboard back.
        Keys.onEscapePressed: {
            if (root.model && root.model.isJumpPending()) {
                root.model.cancelJump()
                return
            }
            filterField.text = ""
            root.filterOpen = false
            root.returnFocus()
        }
        Keys.onDownPressed: tree.forceActiveFocus()
        // Return goes to the first row the filter left, Shift+Return opens it in a window of its own; a row of
        // the window's own then hands the terminal the keyboard (WindowController.activateSidebarFirstMatch).
        Keys.onReturnPressed: (event) => root.acceptFilter(event.modifiers)
        Keys.onEnterPressed: (event) => root.acceptFilter(event.modifiers)

        // The hint, drawn here rather than left to the style as placeholderText, as the command palette's is:
        // every style draws that from where the caret stands, so the caret stood on its first letter.
        Text {
            objectName: "sidebarFilterHint"
            x: filterField.cursorRectangle.x + filterField.cursorRectangle.width + chromeStyle.borderWidth
            width: Math.max(0, filterField.width - x - filterField.rightPadding)
            anchors.verticalCenter: parent.verticalCenter
            visible: filterField.length === 0 && filterField.preeditText === ""
            textFormat: Text.PlainText
            text: qsTr("Filter workspaces")
            font: filterField.font
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarSecondaryInk
            elide: Text.ElideRight
            Accessible.ignored: true
        }
    }

    // The Needs-me toggle of a look that keeps it beside the filter field (root.needsMeInFilterRow).
    SidebarNeedsMeButton {
        id: filterNeedsMeButton
        objectName: "sidebarFilterNeedsMeButton"
        controller: root.controller
        visible: root.needsMeInFilterRow && filterField.visible
        anchors.right: parent.right
        anchors.rightMargin: root.dividerWidth + chromeStyle.space.s3
        anchors.verticalCenter: filterField.verticalCenter
        extent: Math.min(chromeStyle.size.toolbarButton, filterField.implicitHeight)
        implicitWidth: chromeStyle.snapToCells(extent)
    }

    ListView {
        id: tree
        objectName: "sidebarTree"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: root.dividerWidth
        anchors.top: filterField.bottom
        anchors.bottom: footer.top
        clip: true
        model: root.model
        currentIndex: -1
        keyNavigationEnabled: false
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: false

        Accessible.role: Accessible.Tree
        Accessible.name: qsTr("Workspaces")

        // The keys of the focused tree are the controller's to read (window::ChromeKeyRows); the ones the tree
        // answers come back as sidebarCommandRequested.
        Keys.onPressed: (event) => {
            if (root.controller && root.controller.chromeKey)
                event.accepted = root.controller.chromeKey(event.key, event.modifiers, event.text)
        }

        // Rows fold and unfold at once under Reduce Motion.
        add: Transition {
            enabled: chromeStyle.motion.animates
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: chromeStyle.motion.fast }
        }
        // A row displaced while it still fades in has its fade cut short; the displacement finishes it (Qt's
        // "Handling Interrupted Animations"), or the row stays half transparent.
        displaced: Transition {
            enabled: chromeStyle.motion.animates
            NumberAnimation { property: "y"; duration: chromeStyle.motion.fast }
            NumberAnimation { property: "opacity"; to: 1; duration: chromeStyle.motion.fast }
        }

        // Under the Needs-me scope with nothing that needs the user: says so below what the scope still keeps
        // (the shown tab's path), with the way back to every row.
        footer: Column {
            objectName: "sidebarEmptyState"
            readonly property bool shown: !!root.model && root.model.emptyState !== 0
            visible: shown
            width: tree.width
            height: shown ? implicitHeight : 0
            topPadding: chromeStyle.space.s4
            bottomPadding: chromeStyle.space.s4
            spacing: chromeStyle.space.s2

            Text {
                objectName: "sidebarEmptyStateText"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                textFormat: Text.PlainText
                text: qsTr("Nothing needs you")
                font: chromeStyle.type.footnote
                color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarSecondaryInk
                elide: Text.ElideRight
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            // The way back, as a quiet text button: hover and press wear the row's fills.
            Item {
                id: showAll
                objectName: "sidebarEmptyStateShowAll"
                anchors.horizontalCenter: parent.horizontalCenter
                implicitWidth: showAllText.implicitWidth + chromeStyle.space.s3 + chromeStyle.space.s3
                implicitHeight: showAllText.implicitHeight + chromeStyle.space.s2
                Accessible.role: Accessible.Button
                Accessible.name: showAllText.text
                Accessible.onPressAction: showAll.activate()

                function activate() {
                    if (root.model)
                        root.model.setScope(SidebarRows.SidebarScope.All)
                }

                Rectangle {
                    anchors.fill: parent
                    radius: chromeStyle.radii.control
                    color: chromeStyle.paletteRevision < 0 ? "transparent"
                         : showAllTap.pressed ? chromeStyle.colors.controlPressedFill
                         : showAllHover.hovered ? chromeStyle.colors.tabHoverFill
                         : "transparent"
                }

                Text {
                    id: showAllText
                    anchors.centerIn: parent
                    textFormat: Text.PlainText
                    text: qsTr("Show all")
                    font: chromeStyle.type.footnoteStrong
                    color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.tabText
                    Accessible.ignored: true
                }

                HoverHandler { id: showAllHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    id: showAllTap
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: showAll.activate()
                }
            }
        }

        delegate: SidebarRow {
            sidebar: root.model
            controller: root.controller
            keyboardFocused: ListView.isCurrentItem && root.treeFocused
            treeFocused: root.treeFocused
            onPressed: (index) => tree.currentIndex = index
            onRenameEnded: root.returnFocus()
            onHoverCardWanted: (target, wanted) => {
                if (wanted) {
                    hoverCardClose.stop()
                    hoverCard.showFor(target)
                } else {
                    hoverCardClose.restart()
                }
            }
        }
    }

    // The tree's empty area below its last row: a pane or a tab dropped there gets a workspace of its own.
    SidebarNewWorkspaceDrop {
        objectName: "sidebarTreeEmptyDrop"
        sidebar: root.model
        anchors.left: tree.left
        anchors.right: tree.right
        anchors.bottom: tree.bottom
        height: Math.max(0, tree.height - (tree.contentHeight - tree.contentY))
        enabled: height > 0
        radius: 0
    }

    SidebarFooter {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: root.dividerWidth
        anchors.bottom: parent.bottom
        height: implicitHeight
        sidebar: root.model
        controller: root.controller
    }

    SidebarHoverCard {
        id: hoverCard
        sidebar: root.model
        onLeft: hoverCardClose.restart()
    }

    // The card stays while the pointer moves onto it.
    Timer {
        id: hoverCardClose
        interval: chromeStyle.motion.tooltipDelay
        onTriggered: if (!hoverCard.hovered) hoverCard.close()
    }

    // The dividing column, and the handle that resizes the sidebar: the width follows the drag, snapped to
    // whole cells and kept within the row's bounds, and is saved once, when the drag ends.
    Item {
        id: divider
        objectName: "sidebarDivider"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.dividerWidth

        Rectangle {
            visible: chromeStyle.sidebar.dividerGlyph === ""
            anchors.fill: parent
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarDivider
        }

        Column {
            visible: chromeStyle.sidebar.dividerGlyph !== ""
            Repeater {
                model: chromeStyle.sidebar.dividerGlyph !== "" ? Math.ceil(divider.height / chromeStyle.cellHeight) : 0
                Text {
                    width: divider.width
                    height: chromeStyle.cellHeight
                    textFormat: Text.PlainText
                    text: chromeStyle.sidebar.dividerGlyph
                    font: chromeStyle.type.body
                    color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarDivider
                    Accessible.ignored: true
                }
            }
        }

        Item {
            objectName: "sidebarResizeHandle"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Math.max(divider.width, chromeStyle.space.splitHit)

            HoverHandler { cursorShape: Qt.SizeHorCursor }

            DragHandler {
                id: resize
                target: null
                acceptedButtons: Qt.LeftButton
                xAxis.enabled: true
                yAxis.enabled: false
                property real startWidth: 0
                onActiveChanged: {
                    if (active) {
                        resize.startWidth = root.shownWidth
                    } else if (root.liveWidth >= 0) {
                        if (root.controller && root.controller.setSidebarWidth)
                            root.controller.setSidebarWidth(Math.round(root.liveWidth))
                        root.liveWidth = -1
                    }
                }
                onTranslationChanged: {
                    if (active)
                        root.liveWidth = chromeStyle.snappedSidebarWidth(resize.startWidth + translation.x)
                }
            }
        }
    }
}
