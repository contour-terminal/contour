// vim:syntax=qml
// The sidebar's Needs-me control: a toggle that narrows the tree to the rows on the way to what needs the
// user (SidebarModel.scope), and lists everything again on its second press. The ToggleSidebarNeedsMe
// action's way, so a click and its key binding do one thing (WindowController.toggleSidebarNeedsMe). Its name
// and tooltip say what the next press does; checked, it wears the pressed fill. Every place the sidebar's
// controls stand uses this, the sidebar's own title row and the header bar's part above it.
import QtQuick
import Contour.Terminal

SidebarControlButton {
    id: root

    /// The window's controller (WindowController, or a mock in the tests).
    required property var controller

    // A config::SidebarScope's underlying value; All is zero, so anything else is narrowed.
    readonly property bool narrowed: !!root.controller && !!root.controller.sidebar
                                     && root.controller.sidebar.scope !== SidebarRows.SidebarScope.All

    control: "needsMe"
    checkable: true
    checked: root.narrowed
    drawsChecked: true
    // A toolbar item too: the chrome's keyboard walk (Ctrl+F6) visits it.
    focusRingShown: root.activeFocus && !!root.controller && (root.controller.focusedToolbarIndex ?? -1) >= 0
    accessibleName: root.narrowed ? qsTr("Show all") : qsTr("Show only what needs me")
    onClicked: if (root.controller && root.controller.toggleSidebarNeedsMe) root.controller.toggleSidebarNeedsMe()
}
