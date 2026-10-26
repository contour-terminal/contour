// vim:syntax=qml
// The horizontal tab strip, plus on the Flat shape the new-tab split button ("+" and a "▾" profile dropdown).
//
// Under the Pill shape (chromeStyle.layout.tabShape) the tabs sit in a rounded well, every pill as wide
// as chromeStyle.tabWellLayout() says for the width the title bar leaves it: equal widths up to the
// maximum, icon-only pills below the minimum, and a well that scrolls, keeping the active tab in view,
// once even those do not fit. The Flat shape is today's strip, each tab as wide as its title, which scrolls
// the same way once its tabs outgrow the width it is left. Under Pill the new-tab button is the title
// band's (TitleBar's ToolbarButton), so the strip draws neither "+" nor "▾" there.
//
// The model is this window's WindowController (passed in as `controller`), whose rows are the tabs and
// whose roles (title, accentColor, isActive, paneCount) drive each TabItem delegate. Alongside the tabs
// the strip hosts the tabs' one context menu — not a row of the tab model. The new-tab menu is the title
// bar's, which pops it at `newTabMenuAnchor`.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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

    // The QML-facing carve-out of the shape token: whether the tabs are pills in a well.
    readonly property bool pill: chromeStyle.layout.isPill
    // The well's widths for the tabs there are: { tabWidth, wellWidth, label, scrolls }.
    readonly property var wellLayout: chromeStyle.tabWellLayout(root.availableWidth, list.count)

    spacing: 0

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
    onWellLayoutChanged: Qt.callLater(root.revealActiveTab)
    onAvailableWidthChanged: Qt.callLater(root.revealActiveTab)

    // Maps a cursor x (in the drop area's coordinates, what a DropArea's drag reports) to the tab slot
    // the dragged tab would drop into: slot i means "before tab i" (so dropping past the midpoint of tab
    // i lands after it). Returns a value in [0, count]. A scrolled well's tabs sit at their CONTENT
    // positions, so the visible x is shifted by how far the list has scrolled before it is compared.
    // Kept a pure function so the drop math is obvious and reviewable.
    function dropIndexFor(areaX) {
        var x = areaX - list.x + list.contentX
        var count = list.count
        for (var i = 0; i < count; ++i) {
            var item = list.itemAtIndex(i)
            if (!item)
                continue
            if (x < item.x + item.width / 2)
                return i
        }
        return count
    }

    // The content x of the insertion caret for a given drop slot: the left edge of the tab at that
    // slot, or the right edge of the last tab when the slot is past the end.
    function dropIndexToX(index) {
        var count = list.count
        if (count === 0)
            return 0
        if (index >= count) {
            var last = list.itemAtIndex(count - 1)
            return last ? last.x + last.width : 0
        }
        var item = list.itemAtIndex(index)
        return item ? item.x : 0
    }

    // Where the caret for drop slot @p index stands, in the drop area's coordinates: the slot's content
    // position, moved back by the scroll, so the caret is on the visible tab edge it names.
    function caretX(index) {
        return root.dropIndexToX(index) - list.contentX + list.x
    }

    // The tab list plus a DropArea that accepts tabs dragged from this or any other window's strip.
    Item {
        // A Flat strip is as wide as its tabs until they outgrow what the bar leaves them; then it clips
        // and scrolls, so the active tab and the "+" beside it stay in view.
        // A strip on its own, whose width no bar assigns, is as wide as its tabs.
        width: root.pill ? root.wellLayout.wellWidth
             : root.availableWidth > 0 ? Math.min(list.contentWidth, root.availableWidth)
             : list.contentWidth
        height: root.height

        // The well the pills sit in, centred in the bar. Only the Pill shape has one.
        Rectangle {
            id: well
            objectName: "tabWell"   // findChild() handle for the GUI test
            visible: root.pill
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: chromeStyle.size.tabWell
            radius: chromeStyle.radii.tabWell
            color: chromeStyle.colors.tabWellFill
        }

        ListView {
            id: list
            objectName: "tabList" // so offscreen tests can findChild() it
            anchors.fill: root.pill ? well : parent
            // The well's padding at either end and between the pills; the Flat strip sets tabs flush.
            leftMargin: root.pill ? chromeStyle.space.s1 : 0
            rightMargin: root.pill ? chromeStyle.space.s1 : 0
            spacing: root.pill ? chromeStyle.space.s1 : 0
            // Shrink to content width (the Row's implicitWidth then reflects the tabs + button); the
            // parent layout caps the Row, and clip keeps overflow tidy.
            orientation: ListView.Horizontal
            interactive: false
            clip: true
            model: root.controller
            // Null-guarded: when the window closes, the C++ WindowController is destroyed while this
            // QML tree is still alive, so `controller` resets to null and this binding re-evaluates
            // once more before teardown finishes (previously a TypeError in the shutdown log).
            currentIndex: root.controller ? root.controller.activeTabIndex : -1
            onCurrentIndexChanged: Qt.callLater(root.revealActiveTab)
            onCountChanged: Qt.callLater(root.revealActiveTab)

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

                // A pill is the well's width and its own height, centred in the well; a Flat tab is as
                // wide as its title and as tall as the strip.
                height: root.pill ? chromeStyle.size.tab : list.height
                width: root.pill ? root.wellLayout.tabWidth : implicitWidth
                y: root.pill ? (list.height - height) / 2 : 0
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
                onEngagedChanged: root.engagedTabs += engaged ? 1 : -1
                Component.onDestruction: if (engaged) root.engagedTabs -= 1
            }

            // Animate reordering when a tab is moved.
            moveDisplaced: Transition {
                NumberAnimation { properties: "x"; duration: chromeStyle.motion.standard }
            }
        }

        // The insertion caret: a prominent vertical bar shown while a tab is dragged over this strip,
        // parked at the slot boundary where the tab will land (the UX "you'll drop it here" cue). Wide,
        // rounded and opaque so the drop position is unmistakable during a drag.
        Rectangle {
            id: dropIndicator
            width: chromeStyle.dropCaretWidth
            radius: chromeStyle.radius
            height: parent.height
            color: palette.highlight
            visible: dropArea.containsDrag
            // Center the bar on the slot boundary (caretX gives the boundary x).
            x: root.caretX(dropArea.hoverIndex) - width / 2
            z: 2
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

        DropArea {
            id: dropArea
            anchors.fill: parent
            // Accept only Contour tab drags — a keyless DropArea would also light up for file/text OS
            // drags. Qt 6.11 has no Drag.keys on the source, so we key the DropArea on the mime format.
            keys: ["application/x-contour-tab"]
            // The slot the dragged tab would be inserted at, derived from the cursor x over the tabs.
            property int hoverIndex: 0
            onPositionChanged: (drag) => { hoverIndex = root.dropIndexFor(drag.x) }
            onEntered: (drag) => { hoverIndex = root.dropIndexFor(drag.x) }
            onDropped: (drop) => {
                if (!root.controller)
                    return
                // The payload rides in mimeData (drop.source is null across an OS/cross-window QDrag);
                // read it back as a string and parse. hoverIndex already holds the slot for the current
                // cursor position (kept current by onPositionChanged), so reuse it.
                var raw = drop.getDataAsString("application/x-contour-tab")
                if (!raw)
                    return
                var payload = JSON.parse(raw)
                var target = hoverIndex
                if (payload.windowId === root.controller.windowIdValue())
                    root.controller.moveTab(payload.tabIndex, target)  // same window: reorder
                else
                    root.controller.moveTabIntoThisWindow(payload.windowId, payload.tabIndex, target)
                drop.accept(Qt.MoveAction)
            }
        }
    }

    // The new-tab button: on the Flat shape only, since the Pill band draws its own (TitleBar's
    // ToolbarButton). A Loader, so the shape without it has no second "newTabButton" for a test to find.
    Loader {
        id: newTabLoader
        active: !root.pill
        height: root.height
        sourceComponent: ToolButton {
            id: newTabButton
            objectName: "newTabButton"
            height: root.height
            text: chromeStyle.newTabGlyph
            Accessible.name: qsTr("New tab")
            // Wider than a tab's close glyph (see UiStyleTokens::stripButtonUnits): this and the dropdown
            // beside it are permanent, frequently used targets, so the style pads them out.
            width: chromeStyle.stripButtonWidth
            font: chromeStyle.newTabFont
            focusPolicy: Qt.NoFocus
            onClicked: root.controller.createNewTab()
            // Flat, transparent chrome so the button blends into the strip background instead of showing
            // an opaque style button panel; a subtle highlight wash gives hover feedback.
            background: Rectangle {
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
        height: root.height
        sourceComponent: ToolButton {
            id: newTabMenuButton
            objectName: "newTabMenuButton"
            height: root.height
            text: chromeStyle.menuGlyph
            Accessible.name: qsTr("New tab with profile")
            width: chromeStyle.stripButtonWidth
            font: chromeStyle.menuFont
            focusPolicy: Qt.NoFocus
            ToolTip.visible: hovered
            ToolTip.text: qsTr("New tab with profile…")
            onClicked: {
                if (root.controller)
                    root.controller.openNewTabMenu(newTabMenuButton.mapToItem(null, 0, newTabMenuButton.height))
            }
            background: Rectangle {
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
