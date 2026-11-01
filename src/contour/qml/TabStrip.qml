// vim:syntax=qml
// The horizontal tab strip, plus on the Flat shape the new-tab split button ("+" and a "▾" profile dropdown).
//
// Under the Pill shape (chromeStyle.layout.tabShape) the tabs sit in a rounded well, every pill as wide
// as chromeStyle.tabWellLayout() says for the width the title bar leaves it: equal widths up to the
// maximum, icon-only pills below the minimum, and a well that scrolls, keeping the active tab in view,
// once even those do not fit. The Flat shape is today's strip, each tab as wide as its title, which scrolls
// the same way once its tabs outgrow the width it is left. Under Pill the new-tab button is the title
// band's (TitleBar's ToolbarButton), so the strip draws neither "+" nor "▾" there. The Joined shape is the
// Flat strip as the title bar: its tabs stand on the band's floor, and its "+" and "▾" are rounded buttons
// just above it.
//
// A pane or tab dragged over the strip (`application/x-contour-node`) is taken by what it is over: a tab takes
// it for itself (TabItem's DropArea: spring-load to open it, release to land beside its active pane), and the
// gaps between tabs -- and the empty bar after the last -- take it as a tab of its own, showing a dashed
// tab-shaped slot where it would land. Which is which is the controller's decision over the tab spans this
// strip reports to it (tabBarHit).
//
// The model is this window's WindowController (passed in as `controller`), whose rows are the tabs and
// whose roles (title, accentColor, isActive, paneCount) drive each TabItem delegate. Alongside the tabs
// the strip hosts the tabs' one context menu — not a row of the tab model. The new-tab menu is the title
// bar's, which pops it at `newTabMenuAnchor`.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Contour.Terminal

Row {
    id: root

    // A tab strip, so an assistive client reads the tabs below as the pages of one group rather than
    // as loose buttons. The delegates carry the matching PageTab role.
    Accessible.role: Accessible.PageTabList
    Accessible.name: qsTr("Tabs")

    required property var controller
    // The ApplicationWindow, so a delegate can call restoreTerminalFocus() when a rename closes.
    required property var window

    // The width the title bar leaves the tabs: its own width less the window controls, its chips, the drag
    // reserve and this strip's fixed parts (fixedWidth). Assigned by TitleBar.
    property real availableWidth: 0
    // The strip's parts that are not the tabs, which the title bar subtracts from the tabs' width.
    readonly property real fixedWidth: newTabLoader.width + newTabMenuLoader.width
    // The "▾" the title bar pops the new-tab menu at, or null where the strip draws none.
    readonly property Item newTabMenuAnchor: newTabMenuLoader.item
    // How many tabs have their flyout or a rename open; kept by the delegates.
    property int engagedTabs: 0
    /// Whether the strip has a popup or an editor of its own open: the tab menu, or a tab's.
    readonly property bool engaged: tabContextMenu.visible || root.engagedTabs > 0

    // The QML-facing carve-out of the shape token: whether the tabs are pills in a well, or join the content.
    readonly property bool pill: chromeStyle.layout.isPill
    readonly property bool joined: chromeStyle.layout.isJoined
    // Whether the pills sit in a drawn well: not on a row whose well has no height.
    readonly property bool inWell: root.pill && chromeStyle.hasTabWell
    // Where a joined strip's "+" and "▾" stand: the kit's s1 above the band's floor.
    readonly property real joinedButtonY: root.height - chromeStyle.size.stripButton - chromeStyle.space.s1
    // The well's widths for the tabs there are: { tabWidth, wellWidth, label, scrolls }.
    readonly property var wellLayout: chromeStyle.tabWellLayout(root.availableWidth, list.count)

    /// Whether the tabs would have room for the lock: not while the well shows icons alone (TabItem.lockShown).
    /// @param available What the tabs may take, as availableWidth.
    function hostsLockWithin(available) {
        return !root.pill || chromeStyle.tabWellLayout(available, list.count).label !== "iconOnly";
    }

    spacing: 0

    // The keys of the tab well, offered to the controller while a tab holds keyboard focus (TabItem takes it
    // from focusTab()): a key the controller handles is accepted here, so it never reaches the shell. The
    // focused tab's own editor, when one is open, takes its keys first and never lets them up.
    Keys.onPressed: (event) => {
        if (root.controller && root.controller.chromeKey)
            event.accepted = root.controller.chromeKey(event.key, event.modifiers)
    }

    /// Gives keyboard focus to the tab at @p index, bringing it into view: how the controller's chrome focus
    /// reaches a real active-focus item. Does nothing for a row that has no delegate.
    function focusTab(index) {
        list.positionViewAtIndex(index, ListView.Contain)
        const tab = list.itemAtIndex(index)
        if (tab)
            tab.forceActiveFocus()
    }

    // Brings the active tab into view, should the tabs have scrolled it out: the well's, or a Flat strip
    // that outgrew its width. Deferred, because it runs on changes the list has not laid out yet.
    //
    // positionViewAtIndex ends in Flickable's fixup, which settles a fractional contentX onto a whole pixel
    // in either direction -- and a tab's edges are fractional wherever a cell or a text width is, so the
    // tab could be left up to a pixel outside the strip. The scroll is rounded outward here instead: far
    // enough that the whole tab is in view, onto a whole pixel the fixup then leaves alone.
    function revealActiveTab() {
        if (!root.controller)
            return
        const index = root.controller.activeTabIndex
        list.positionViewAtIndex(index, ListView.Contain)
        const tab = list.itemAtIndex(index)
        if (!tab)
            return
        if (tab.x + tab.width > list.contentX + list.width)
            list.contentX = Math.ceil(tab.x + tab.width - list.width)
        if (tab.x < list.contentX)
            list.contentX = Math.floor(tab.x)
    }
    onWellLayoutChanged: {
        Qt.callLater(root.revealActiveTab)
        root.scheduleSpans()
    }
    onAvailableWidthChanged: {
        Qt.callLater(root.revealActiveTab)
        root.scheduleSpans()
    }

    /// How wide the insertion slot is.
    readonly property real slotWidth: 110
    /// The gap the insertion slot stands in while a node is dragged over one; -1 while there is none.
    readonly property int slotGap: dropArea.containsDrag ? dropArea.gapIndex : -1
    /// How far the tabs from the slot's gap on move right to make room for it: the slot and the spacing after
    /// it. Nothing for the slot after the last tab, which has the empty bar to stand in.
    readonly property real roomMade: slotGap >= 0 && slotGap < list.count ? slotWidth + list.spacing : 0
    onRoomMadeChanged: root.scheduleSpans()
    onSlotGapChanged: root.scheduleSpans()
    /// How much wider a strip that is only as wide as its tabs becomes for the slot, so that the "+" beside it
    /// moves on instead of standing under the slot: the room the tabs make between two of them, or the slot
    /// itself after the last. A strip that fills the bar has the empty bar to stand in.
    readonly property real slotRoom: root.slotGap < 0 ? 0
                                   : root.slotGap < list.count ? root.roomMade
                                   : root.slotWidth + chromeStyle.space.s2

    // Where the tabs stand, reported to the controller as a drag enters the strip and whenever the strip lays
    // out or scrolls while the drag is over it -- the only time anything hit-tests against them (tabBarHit) --
    // in the coordinates of the strip's drop area (with the room the slot takes, so what the pointer is over
    // is what the controller hit-tests it against). One span per tab, a tab with no delegate (scrolled far
    // out of view) a zero-width span at the edge of the one before it.
    function reportSpans() {
        if (!root.controller)
            return
        const spans = []
        let edge = listClip.x + list.x - list.contentX
        for (let i = 0; i < list.count; ++i) {
            // As they stand once the tabs have made room for the slot, not as they animate there.
            const shift = i >= root.slotGap ? root.roomMade : 0
            const tab = list.itemAtIndex(i)
            if (tab) {
                edge = listClip.x + list.x + tab.x - list.contentX + shift
                spans.push(Qt.rect(edge, 0, tab.width, tab.height))
                edge += tab.width
            } else {
                spans.push(Qt.rect(edge, 0, 0, 0))
            }
        }
        root.controller.setTabSpans(spans)
    }
    function scheduleSpans() {
        // Over the strip from its entry (which reports at once) to its exit or drop.
        if (dropArea.nodePayload !== "")
            Qt.callLater(root.reportSpans)
    }

    // Where the insertion slot for gap @p index stands, in the drop area's coordinates: centred on the
    // boundary between two tabs, and running on from the last tab at the end.
    // @param index The gap, as tabBarHit() names it: before the tab at that row, or after the last.
    function slotX(index) {
        const count = list.count
        if (count === 0)
            return listClip.x
        if (index >= count) {
            const last = list.itemAtIndex(count - 1)
            return last ? listClip.x + list.x + last.x + last.width - list.contentX + chromeStyle.space.s2
                        : listClip.x
        }
        // Where the tab there stands unmoved: the room the other tabs make is this one's.
        const tab = list.itemAtIndex(index)
        return tab ? listClip.x + list.x + tab.x - list.contentX : listClip.x
    }

    // The tab list plus a DropArea that accepts tabs dragged from this or any other window's strip.
    Item {
        objectName: root.joined ? "tabShapeJoined" : ""
        // A Flat strip is as wide as its tabs until they outgrow what the bar leaves them; then it clips
        // and scrolls, so the active tab and the "+" beside it stay in view.
        // A strip on its own, whose width no bar assigns, is as wide as its tabs.
        width: root.pill ? root.wellLayout.wellWidth
             : root.availableWidth > 0 ? Math.min(list.contentWidth + root.slotRoom, root.availableWidth)
             : list.contentWidth + root.slotRoom
        height: root.height

        // The well the pills sit in, centred in the bar. Only the Pill shape has one, and only on a row whose
        // well has a height: ContourLinux's header bar sets its pills on the bar itself.
        Rectangle {
            id: well
            objectName: "tabWell"   // findChild() handle for the GUI test
            visible: root.inWell
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: chromeStyle.size.tabWell
            radius: chromeStyle.radii.tabWell
            color: chromeStyle.colors.tabWellFill
        }

        // What the tabs are clipped to: the well, or the strip. A pill's focus ring reaches past the pill, into
        // the well's own margin and a little beyond it, so the clip overhangs the well by the ring, or it
        // would cut the ring's top and bottom. The list inside it is clipped by this, not by itself.
        Item {
            id: listClip
            anchors.fill: root.inWell ? well : parent
            anchors.topMargin: root.pill ? -chromeStyle.size.focusRing : 0
            anchors.bottomMargin: root.pill ? -chromeStyle.size.focusRing : 0
            clip: true

            ListView {
                id: list
                objectName: "tabList" // so offscreen tests can findChild() it
                // As tall as a tab wherever a tab is shorter than the strip: a horizontal ListView puts every
                // delegate it positions back at y 0, so a tab cannot centre itself in a taller list -- the list
                // is placed instead. Pills are centred in the clip (and so in the well); joined tabs stand on
                // the band's floor, at the strip's bottom; a Flat tab is the strip's height.
                anchors.left: parent.left
                anchors.right: parent.right
                height: root.pill || root.joined ? chromeStyle.size.tab : parent.height
                y: root.joined ? parent.height - height : (parent.height - height) / 2
                // The well's padding at either end and between the pills (chromeStyle.wellPadding, wellGap -- the
                // same the well's widths are laid out with); the Flat strip sets tabs flush.
                leftMargin: root.pill ? chromeStyle.wellPadding : 0
                rightMargin: root.pill ? chromeStyle.wellPadding : 0
                spacing: root.pill ? chromeStyle.wellGap : 0
                // Shrink to content width (the Row's implicitWidth then reflects the tabs + button); the
                // parent layout caps the Row, and listClip keeps overflow tidy.
                orientation: ListView.Horizontal
                interactive: false
                model: root.controller
                // Null-guarded: when the window closes, the C++ WindowController is destroyed while this
                // QML tree is still alive, so `controller` resets to null and this binding re-evaluates
                // once more before teardown finishes (previously a TypeError in the shutdown log).
                currentIndex: root.controller ? root.controller.activeTabIndex : -1
                onCurrentIndexChanged: Qt.callLater(root.revealActiveTab)
                onCountChanged: {
                    Qt.callLater(root.revealActiveTab)
                    root.scheduleSpans()
                }
                onContentXChanged: root.scheduleSpans()

                delegate: TabItem {
                    // Role values injected for this row. Declared as required properties (the modern
                    // delegate model); their names differ from this TabItem's own properties so there is
                    // no shadowing.
                    required property int index
                    required property string title
                    required property string rawTitle
                    required property color accentColor
                    required property bool isActive
                    required property int paneCount
                    required property bool zoomed
                    required property int progressState
                    required property int progressPercentage
                    required property int badge
                    required property string badgeText
                    required property int badgeProgress
                    // Read through the model object, not as a required role: a model without the tabSecure role
                    // (WindowController's SecureRole) shows no lock rather than failing the delegate.
                    required property var model

                    // A pill is the well's width, a Flat or joined tab as wide as its title; each is as tall as
                    // the list, which is placed where the tabs stand.
                    height: list.height
                    width: root.pill ? root.wellLayout.tabWidth : implicitWidth
                    labelMode: root.pill ? root.wellLayout.label : "full"
                    controller: root.controller
                    window: root.window
                    tabIndex: index
                    tabTitle: title
                    tabRawTitle: rawTitle
                    tabColor: accentColor
                    tabActive: isActive
                    tabPaneCount: paneCount
                    tabZoomed: zoomed
                    tabProgressState: progressState
                    tabProgressPercentage: progressPercentage
                    tabBadge: badge
                    tabBadgeText: badgeText
                    tabBadgeProgress: badgeProgress
                    locked: (model.tabSecure ?? 0) !== 0
                    secureState: model.tabSecure ?? 0
                    // The tabs from the slot's gap on make room for it, animated as a reorder is.
                    transform: Translate {
                        x: index >= root.slotGap ? root.roomMade : 0
                        Behavior on x {
                            NumberAnimation { duration: chromeStyle.motion.standard }
                        }
                    }
                    onXChanged: root.scheduleSpans()
                    onWidthChanged: root.scheduleSpans()
                    onEngagedChanged: root.engagedTabs += engaged ? 1 : -1
                    Component.onDestruction: if (engaged) root.engagedTabs -= 1
                }

                // Animate reordering when a tab is moved.
                moveDisplaced: Transition {
                    NumberAnimation { properties: "x"; duration: chromeStyle.motion.standard }
                }
            }
        }

        // Horizontal wheel over the tabs switches tabs. A MouseArea rather than a WheelHandler: both
        // axes are needed in one callback (the vertical one tells a sideways gesture from the sideways
        // drift of a vertical one), and acceptedButtons: Qt.NoButton leaves every click, hover and drag
        // in the TabItem delegates below completely untouched.
        //
        // Scoped to this Item, which is sized to the tabs' content width -- so the gesture works over the
        // tabs themselves and does not swallow the wheel over the "+" button beside them.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            z: 1
            onWheel: (wheel) => {
                // Null-guarded like the bindings above: the C++ controller is destroyed while this QML
                // tree is still alive during window teardown.
                if (root.controller)
                    root.controller.dispatchTabStripWheel(wheel.pixelDelta.x, wheel.pixelDelta.y,
                                                          wheel.angleDelta.x, wheel.angleDelta.y,
                                                          wheel.phase, wheel.inverted)
                wheel.accepted = true
            }
        }

        // The gaps between the tabs, before the first and after the last, and the empty bar beyond: a node
        // dropped there becomes a tab of its own at that slot (a tab of this window's is reordered to it).
        // The tabs' own DropAreas lie above this one, over the middle of each tab.
        DropArea {
            id: dropArea
            objectName: "tabGapDropArea"
            // Beneath the tabs' own DropAreas, which take the middle of each tab.
            z: -1
            x: 0
            y: 0
            width: Math.max(parent.width, root.availableWidth)
            height: parent.height
            // Accept only Contour node drags — a keyless DropArea would also light up for file/text OS
            // drags. Qt 6.11 has no Drag.keys on the source, so we key the DropArea on the mime format.
            keys: [NodeDrag.mime]
            /// The drag's JSON, and the gap it is over: -1 when it is over no gap.
            property string nodePayload: ""
            property int gapIndex: -1
            /// What the slot names: the dragged pane's or tab's title.
            property string title: ""

            function locate(drag) {
                const hit = root.controller ? root.controller.tabBarHit(drag.x) : undefined
                gapIndex = hit !== undefined && hit.spot === TabBarSpot.Gap ? hit.index : -1
            }

            onEntered: (drag) => {
                nodePayload = drag.getDataAsString(NodeDrag.mime)
                title = root.controller ? root.controller.dragTitle(nodePayload) : ""
                root.reportSpans()
                locate(drag)
                drag.accepted = true
            }
            onPositionChanged: (drag) => {
                locate(drag)
                drag.accepted = true
            }
            onExited: {
                gapIndex = -1
                nodePayload = ""
            }
            onDropped: (drop) => {
                locate(drop)
                const payload = drop.getDataAsString(NodeDrag.mime)
                const gap = gapIndex
                gapIndex = -1
                nodePayload = ""
                if (root.controller && gap >= 0)
                    root.controller.dropNodeInGap(gap, payload)
                // Every drop recognised is accepted, or the drag would end as a drop outside every window and
                // open one.
                drop.accept(Qt.MoveAction)
            }
        }

        // The slot: a dashed, tab-shaped outline in the accent where the dropped node would become a tab,
        // named for it. Between two tabs they have moved apart for it (roomMade), so it overlaps no title.
        Item {
            id: insertionSlot
            objectName: "tabInsertionSlot"
            z: 2
            visible: dropArea.containsDrag && dropArea.gapIndex >= 0
            width: root.slotWidth
            height: list.height
            x: root.slotX(dropArea.gapIndex)
            y: listClip.y + list.y + list.height - height
            /// The slot's edge and tint: the chrome's accent, as every "lands here" cue is.
            readonly property color mark: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.accentColor

            Rectangle {
                anchors.fill: parent
                radius: chromeStyle.radii.tab
                color: Qt.alpha(insertionSlot.mark, 0.18)
            }
            DashedFrame {
                anchors.fill: parent
                cornerInset: chromeStyle.radii.tab
                color: insertionSlot.mark
            }
            Text {
                objectName: "tabInsertionSlotLabel"
                anchors.centerIn: parent
                width: parent.width - chromeStyle.space.s4 - chromeStyle.space.s4
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: dropArea.title !== "" ? dropArea.title : qsTr("New tab")
                font: chromeStyle.activeTabFont(chromeStyle.type.tab)
                color: chromeStyle.colors.tabText
            }
        }
    }

    // The new-tab button: on the Flat shape only, since the Pill band draws its own (TitleBar's
    // ToolbarButton). A Loader, so the shape without it has no second "newTabButton" for a test to find.
    Loader {
        id: newTabLoader
        active: !root.pill
        y: root.joined ? root.joinedButtonY : 0
        height: root.joined ? chromeStyle.size.stripButton : root.height
        sourceComponent: ToolButton {
            id: newTabButton
            objectName: "newTabButton"
            height: newTabLoader.height
            text: chromeStyle.newTabGlyph
            Accessible.name: qsTr("New tab")
            // Wider than a tab's close glyph (see UiStyleTokens::stripButtonUnits): this and the dropdown
            // beside it are permanent, frequently used targets, so the style pads them out.
            width: chromeStyle.stripButtonWidth
            font: chromeStyle.newTabFont
            focusPolicy: Qt.NoFocus
            onClicked: root.controller.createNewTab()
            KitToolTip {
                objectName: "newTabToolTip"
                text: qsTr("New tab")
                shortcutCommand: "CreateNewTab"
                shortcutSource: root.controller
                wanted: newTabButton.hovered
            }
            // Flat, transparent chrome so the button blends into the strip background instead of showing
            // an opaque style button panel; a subtle highlight wash gives hover feedback.
            background: Rectangle {
                radius: root.joined ? chromeStyle.radii.control : 0
                color: newTabButton.hovered ? chromeStyle.wash(newTabButton.palette.highlight)
                                            : "transparent"
            }
        }
    }

    // The new-tab profile dropdown: the "▾" beside "+", where the row reaches the profiles by a caret
    // (layout.profileMenu). Asks the controller, which publishes one entry per configured profile (open a new
    // tab with it) and asks for the menu, which the title bar pops below this button — the Windows-Terminal
    // split-button idiom.
    Loader {
        id: newTabMenuLoader
        active: !root.pill && chromeStyle.layout.profileMenu === chromeStyle.layout.caret
        y: root.joined ? root.joinedButtonY : 0
        height: root.joined ? chromeStyle.size.stripButton : root.height
        sourceComponent: ToolButton {
            id: newTabMenuButton
            objectName: "newTabMenuButton"
            height: newTabMenuLoader.height
            text: chromeStyle.menuGlyph
            Accessible.name: qsTr("New tab with profile")
            // The joined strip's profile button is narrower than its "+" (the win board's 28 beside 36).
            width: root.joined ? chromeStyle.size.profileButton : chromeStyle.stripButtonWidth
            font: chromeStyle.menuFont
            focusPolicy: Qt.NoFocus
            KitToolTip {
                objectName: "newTabMenuToolTip"
                text: qsTr("New tab with profile…")
                wanted: newTabMenuButton.hovered
            }
            onClicked: {
                if (root.controller)
                    root.controller.openNewTabMenu(newTabMenuButton.mapToItem(null, 0, newTabMenuButton.height))
            }
            background: Rectangle {
                radius: root.joined ? chromeStyle.radii.control : 0
                color: newTabMenuButton.hovered ? chromeStyle.wash(newTabMenuButton.palette.highlight)
                                                : "transparent"
            }
        }
    }

    // The context menu of every tab: ONE menu for the strip, not one per delegate, rebuilt once per
    // publish. The controller publishes the right-clicked tab's rows, then asks for the menu with that
    // tab's index; it opens below that tab's delegate rather than at the live pointer, so where it opens
    // does not depend on where the mouse happens to be.
    ActionContextMenu {
        id: tabContextMenu
        objectName: "tabContextMenu"
        entries: root.controller ? root.controller.tabContextMenuModel : []
        onPicked: (actionId) => { if (root.controller) root.controller.triggerTabContextMenuAction(actionId) }
    }

    Connections {
        target: root.controller
        function onTabContextMenuRequested(index) {
            // A delegate exists only once the list is laid out, and only while it is in view: lay it out
            // first, and bring a tab scrolled out of the strip back into view, so the menu still opens
            // under the tab it names.
            list.forceLayout()
            if (!list.itemAtIndex(index))
                list.positionViewAtIndex(index, ListView.Contain)
            const item = list.itemAtIndex(index)
            if (item)
                tabContextMenu.popup(item, 0, item.height)
            else
                tabContextMenu.popup(list, 0, list.height)
        }
    }
}
