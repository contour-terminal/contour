// vim:syntax=qml
// The sidebar's switcher control: drops the daemon and workspace switcher down below itself, as the location
// chip did before the tree named the daemon in its heading row (WindowController.showWorkspaceDropdown). Its
// tooltip names the ShowSwitcher chord bound now. Only a daemon's window carries it
// (WindowController.chip.switcherControlShown); a tmux window's chip still opens the switcher. A toolbar item:
// the chrome's keyboard walk (Ctrl+F6) visits it, and Return and Space press it.
import QtQuick

SidebarControlButton {
    id: root

    /// The window's controller (WindowController, or a mock in the tests).
    required property var controller

    /// Whether the window carries the control: a daemon's does. Where a row has no room, it may still hide it.
    readonly property bool wanted: !!root.controller && !!root.controller.chip
                                   && root.controller.chip.switcherControlShown

    control: "switcher"
    visible: root.wanted
    accessibleName: qsTr("Switch workspace or daemon")
    focusRingShown: root.activeFocus && !!root.controller && (root.controller.focusedToolbarIndex ?? -1) >= 0
    onClicked: if (root.controller && root.controller.showWorkspaceDropdown)
        root.controller.showWorkspaceDropdown(root.mapToItem(null, 0, 0, root.width, root.height))
}
