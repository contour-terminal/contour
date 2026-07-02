// vim:syntax=qml
// One tab in the title-bar tab strip.
//
// Shows the tab title, an accent bar in the tab's color, and a close button. Single-click
// activates the tab; double-click starts an inline rename; right-click opens the context menu.
// All mutations go through the `controller` (the TerminalSessionManager exposed to QML as
// `terminalSessions`), which forwards them to the authoritative vtmux model.
import QtQuick
import QtQuick.Controls

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

    implicitWidth: Math.min(240, Math.max(120, label.implicitWidth + 56))
    implicitHeight: 32

    // Live OS palette handle so the tab adapts to dark/light in realtime (see TabContextMenu).
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Background + active highlight. The active tab uses the OS highlight color; hover is a faint
    // wash of the highlight color over the (transparent) bar. Blending relative to a palette role —
    // rather than a fixed white overlay — keeps the affordance visible in both dark and light themes.
    Rectangle {
        anchors.fill: parent
        color: root.tabActive
                   ? systemPalette.highlight
                   : (hover.hovered ? Qt.rgba(systemPalette.highlight.r,
                                              systemPalette.highlight.g,
                                              systemPalette.highlight.b,
                                              0.25)
                                    : "transparent")
    }

    // Accent bar in the tab's color, along the bottom edge.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 2
        color: root.tabColor
        visible: root.tabColor.a > 0
    }

    HoverHandler { id: hover }

    Text {
        id: label
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.right: closeButton.left
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        text: root.tabTitle
        // The active tab paints the OS highlight background, so its label uses highlightedText for
        // contrast; inactive tabs sit on the (themed) title-bar window color and use windowText.
        color: root.tabActive ? systemPalette.highlightedText : systemPalette.windowText
        elide: Text.ElideRight
        visible: !renameLoader.active
    }

    // Inline rename field, shown on double-click.
    Loader {
        id: renameLoader
        active: false
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.right: closeButton.left
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
        // Color the glyph from the palette so it matches the label and stays visible in light mode
        // (the Basic ToolButton's default text color does not track the active-tab highlight).
        contentItem: Text {
            text: closeButton.text
            font: closeButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: root.tabActive ? systemPalette.highlightedText : systemPalette.windowText
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
    }
}
