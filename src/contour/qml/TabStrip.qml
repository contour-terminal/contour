// vim:syntax=qml
// The horizontal tab strip plus the new-tab split button ("+" and a "▾" profile dropdown).
//
// The model is this window's WindowController (passed in as `controller`), whose rows are the tabs and
// whose roles (title, accentColor, isActive, paneCount) drive each TabItem delegate. Alongside the tabs
// the strip hosts the tabs' one context menu and the new-tab dropdown — neither a row of the tab model.
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

    spacing: 0

    // Maps a cursor x (in list content coordinates) to the tab slot the dragged tab would drop into:
    // slot i means "before tab i" (so dropping past the midpoint of tab i lands after it). Returns a
    // value in [0, count]. Kept a pure function so the drop math is obvious and reviewable.
    function dropIndexFor(x) {
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

    // The x position of the insertion caret for a given drop slot: the left edge of the tab at that
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

    // The remote host this window's sessions run on (`contour client --host`), at the strip's leading edge,
    // so a remote window is never mistaken for a local one. Not interactive, and in the inactive tab text's
    // font and colour: it names where the tabs are, it is not one of them. Absent for a local window.
    Item {
        id: hostBadge
        objectName: "hostBadge"
        // `?? ""` keeps a lightweight mock controller (offscreen tests) without the property local.
        readonly property string hostLabel: root.controller ? (root.controller.hostLabel ?? "") : ""
        visible: hostLabel !== ""
        width: visible ? hostBadgeText.implicitWidth + (2 * chromeStyle.labelPadding) : 0
        height: root.height
        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("Attached to %1").arg(hostLabel)

        SystemPalette {
            id: hostBadgePalette
            colorGroup: SystemPalette.Active
        }

        Text {
            id: hostBadgeText
            objectName: "hostBadgeText"
            anchors.centerIn: parent
            font: chromeStyle.font
            text: hostBadge.hostLabel
            color: hostBadgePalette.windowText
        }
    }

    // The tab list plus a DropArea that accepts tabs dragged from this or any other window's strip.
    Item {
        width: list.contentWidth
        height: root.height

        ListView {
            id: list
            objectName: "tabList" // so offscreen tests can findChild() it
            anchors.fill: parent
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

                height: list.height
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
            }

            // Animate reordering when a tab is moved.
            moveDisplaced: Transition {
                NumberAnimation { properties: "x"; duration: 120 }
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
            // Center the bar on the slot boundary (dropIndexToX gives the boundary x).
            x: root.dropIndexToX(dropArea.hoverIndex) - width / 2
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

    ToolButton {
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

    // The new-tab profile dropdown: the "▾" beside "+". Asks the controller, which publishes one entry per
    // configured profile (open a new tab with it) and asks for the menu below — the Windows-Terminal
    // split-button idiom.
    ToolButton {
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

        ActionContextMenu {
            id: newTabMenu
            objectName: "newTabMenu"
            entries: root.controller ? root.controller.newTabMenuModel : []
            onPicked: (actionId) => { if (root.controller) root.controller.triggerNewTabMenuAction(actionId) }
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
        function onNewTabMenuRequested() {
            newTabMenu.popup(newTabMenuButton, 0, newTabMenuButton.height)
        }
    }
}
