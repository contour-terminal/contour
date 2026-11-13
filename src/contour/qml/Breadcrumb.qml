// vim:syntax=qml
// The title band's quiet breadcrumb while the sidebar holds the tabs: `workspace › tab`, the workspace the
// window shows (SidebarModel.currentWorkspaceTitle) and the active tab's label. The tab gives way first when
// the band is narrow; a screen reader hears it as one line.
import QtQuick

Row {
    id: root
    objectName: "breadcrumb"

    /// The shown workspace's name.
    property string workspace
    /// The active tab's label.
    property string tab
    /// The room the breadcrumb may take.
    property real room: 0
    /// The ink, on the band it sits on.
    property color ink: chromeStyle.colors.tabText

    // At least a cell on the Terminal look, where the space tokens are zero, so the parts never touch.
    spacing: chromeStyle.sidebar.look === chromeStyle.sidebar.cellTree
             ? Math.max(chromeStyle.space.s3, chromeStyle.cellWidth) : chromeStyle.space.s3

    Accessible.role: Accessible.StaticText
    Accessible.name: root.workspace !== "" ? qsTr("%1, %2").arg(root.workspace).arg(root.tab) : root.tab

    Text {
        id: workspaceText
        objectName: "breadcrumbWorkspace"
        visible: root.workspace !== ""
        width: Math.min(implicitWidth, root.room)
        textFormat: Text.PlainText
        text: root.workspace
        font: chromeStyle.type.bodyMedium
        color: root.ink
        elide: Text.ElideRight
        Accessible.ignored: true
    }

    Text {
        id: separator
        objectName: "breadcrumbSeparator"
        visible: workspaceText.visible
        textFormat: Text.PlainText
        text: chromeStyle.breadcrumbGlyph
        font: chromeStyle.type.bodyMedium
        color: root.ink
        Accessible.ignored: true
    }

    Text {
        objectName: "breadcrumbTab"
        width: Math.min(implicitWidth, Math.max(0, root.room - (workspaceText.visible
                                                                ? workspaceText.width + separator.width
                                                                  + root.spacing + root.spacing : 0)))
        textFormat: Text.PlainText
        text: root.tab
        font: chromeStyle.type.bodyMedium
        color: root.ink
        elide: Text.ElideMiddle
        Accessible.ignored: true
    }
}
