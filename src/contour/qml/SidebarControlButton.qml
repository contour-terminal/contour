// vim:syntax=qml
// One of the workspace sidebar's own buttons -- its toggle, its new-workspace button, its filter button --
// drawn as this chrome row draws it (UiStyleProvider.sidebarControls, window::SidebarControlRows): a themed
// symbolic icon in the ContourLinux header bar, a bundled icon on macOS and Fluent, a glyph in cells on the
// Terminal row. Every place a control stands uses this, so it looks the same wherever it moves.
import QtQuick

ToolbarButton {
    id: root

    /// Which control: "toggle", "newWorkspace" or "filter".
    required property string control
    /// The window's sidebar model, on a New Workspace control: a pane or a tab dropped on it gets a workspace of
    /// its own (SidebarNewWorkspaceDrop). Null on every other control, and where no sidebar is.
    property var dropSidebar: null

    readonly property var face: chromeStyle.sidebarControls[root.control] ?? ({})

    glyph: root.face.glyph ?? ""
    iconName: root.face.iconName ?? ""
    iconSource: root.face.bundledIcon ?? ""
    // The control's action, from the table: the tooltip names the chord it is bound to when it shows.
    toolTipCommand: root.face.command ?? ""

    SidebarNewWorkspaceDrop {
        anchors.fill: parent
        sidebar: root.dropSidebar
        enabled: !!root.face.takesNewWorkspaceDrop && !!root.dropSidebar
    }
}
