// vim:syntax=qml
// The horizontal tab strip plus the new-tab ("+") button.
//
// The model is the TerminalSessionManager (exposed to QML as `terminalSessions`), whose rows are
// the tabs and whose roles (title, accentColor, isActive, paneCount) drive each TabItem delegate.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Row {
    id: root

    required property var controller
    // The ApplicationWindow, so a delegate can call restoreTerminalFocus() when a rename closes.
    required property var window

    spacing: 0

    ListView {
        id: list
        height: root.height
        // Shrink to content width (the Row's implicitWidth then reflects the tabs + button); the
        // parent layout caps the Row, and clip keeps overflow tidy.
        width: contentWidth
        orientation: ListView.Horizontal
        interactive: false
        clip: true
        model: root.controller
        currentIndex: root.controller.activeTabIndex

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

            height: list.height
            controller: root.controller
            window: root.window
            tabIndex: index
            tabTitle: title
            tabRawTitle: rawTitle
            tabColor: accentColor
            tabActive: isActive
            tabPaneCount: paneCount
        }

        // Animate reordering when a tab is moved.
        moveDisplaced: Transition {
            NumberAnimation { properties: "x"; duration: 120 }
        }
    }

    ToolButton {
        id: newTabButton
        height: root.height
        text: "+"
        font.pointSize: 12
        focusPolicy: Qt.NoFocus
        onClicked: root.controller.createNewTab()
    }
}
