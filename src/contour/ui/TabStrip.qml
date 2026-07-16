// vim:syntax=qml
// The horizontal tab strip plus the new-tab split button ("+" and a "▾" profile dropdown).
//
// The model is this window's WindowController (passed in as `controller`), whose rows are the tabs and
// whose roles (title, accentColor, isActive, paneCount) drive each TabItem delegate. Alongside the tabs
// the strip hosts a Qt-layer settings "tab" (shown while the settings page is open) and the new-tab
// dropdown (NewTabMenu) — neither is a row of the tab model.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Row {
    id: root

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

    // The tab list plus a DropArea that accepts tabs dragged from this or any other window's strip.
    Item {
        width: list.contentWidth
        height: root.height

        ListView {
            id: list
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
            width: 4
            radius: 2
            height: parent.height
            color: palette.highlight
            visible: dropArea.containsDrag
            // Center the bar on the slot boundary (dropIndexToX gives the boundary x).
            x: root.dropIndexToX(dropArea.hoverIndex) - width / 2
            z: 2
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

    // The settings "tab": shown in the strip while the settings page is open (settingsActive), reading
    // and behaving like a tab — highlighted as the active view, with a close button that leaves settings.
    // It is a Qt-layer affordance (vtmux untouched), so it is a pinned item here rather than a model row.
    Item {
        id: settingsTab
        objectName: "settingsTab"
        height: root.height
        // `=== true` coerces a missing/undefined property (a lightweight mock controller in the offscreen
        // tests) to a real bool, so the binding never assigns undefined.
        visible: root.controller !== null && root.controller.settingsActive === true
        width: visible ? settingsTabRow.implicitWidth + 20 : 0

        SystemPalette {
            id: settingsTabPalette
            colorGroup: SystemPalette.Active
        }

        Rectangle {
            anchors.fill: parent
            // It exists only while active, so it always wears the active (OS highlight) fill.
            color: settingsTabPalette.highlight
        }

        Row {
            id: settingsTabRow
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 10
            spacing: 6

            Label {
                text: qsTr("⚙ Settings")
                color: settingsTabPalette.highlightedText
                anchors.verticalCenter: parent.verticalCenter
            }
            ToolButton {
                id: settingsTabClose
                objectName: "settingsTabClose"
                width: 20
                height: 20
                anchors.verticalCenter: parent.verticalCenter
                focusPolicy: Qt.NoFocus
                onClicked: if (root.controller) root.controller.closeSettings()
                contentItem: Label {
                    text: "✕"
                    color: settingsTabPalette.highlightedText
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 3
                    color: settingsTabClose.hovered
                           ? Qt.rgba(settingsTabPalette.highlightedText.r,
                                     settingsTabPalette.highlightedText.g,
                                     settingsTabPalette.highlightedText.b, 0.25)
                           : "transparent"
                }
            }
        }
    }

    ToolButton {
        id: newTabButton
        height: root.height
        text: "+"
        font.pointSize: 12
        focusPolicy: Qt.NoFocus
        onClicked: root.controller.createNewTab()
        // Flat, transparent chrome so the button blends into the strip background instead of showing
        // an opaque style button panel; a subtle highlight wash gives hover feedback.
        background: Rectangle {
            color: newTabButton.hovered ? Qt.rgba(newTabButton.palette.highlight.r,
                                                  newTabButton.palette.highlight.g,
                                                  newTabButton.palette.highlight.b,
                                                  0.25)
                                        : "transparent"
        }
    }

    // The new-tab profile dropdown: the "▾" beside "+". Opens NewTabMenu — one entry per configured
    // profile (open a new tab with it) plus a "Settings" entry — the Windows-Terminal split-button idiom.
    ToolButton {
        id: newTabMenuButton
        objectName: "newTabMenuButton"
        height: root.height
        text: "▾"
        font.pointSize: 10
        focusPolicy: Qt.NoFocus
        ToolTip.visible: hovered
        ToolTip.text: qsTr("New tab with profile…")
        onClicked: newTabMenu.popup(newTabMenuButton, 0, newTabMenuButton.height)
        background: Rectangle {
            color: newTabMenuButton.hovered ? Qt.rgba(newTabMenuButton.palette.highlight.r,
                                                      newTabMenuButton.palette.highlight.g,
                                                      newTabMenuButton.palette.highlight.b,
                                                      0.25)
                                            : "transparent"
        }

        NewTabMenu {
            id: newTabMenu
            controller: root.controller
        }
    }
}
