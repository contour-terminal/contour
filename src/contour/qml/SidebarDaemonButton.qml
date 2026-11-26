// vim:syntax=qml
// One button of a daemon row in the sidebar -- Connect, Start, Cancel or Remove (window::DaemonRowButton) --
// as the kit's ToolbarButton: its words while the row has room for them (in brackets on the Terminal row),
// else its icon (a one-cell glyph on the Terminal row), its words then its accessible name and tooltip.
// Remove, which has no icon form, keeps its words at every width: it gives way before the buttons shrink.
// What the button looks like is the table's (UiStyleProvider.daemonRowButton, window::DaemonRowButtonFaces);
// what it does is the model's (SidebarModel.pressDaemonButton).
import QtQuick
import Contour.Terminal

ToolbarButton {
    id: root

    /// Which button: a window::DaemonRowButton's underlying value.
    required property int button
    /// Whether a jump into the row's section is under way (SidebarRows.RowPending): Cancel then runs the
    /// CancelSwitch action and its tooltip names that action's chord; otherwise it ends a connect, and names none.
    property int pending: SidebarRows.RowPending.No
    /// Whether the row's room asks the button for its icon alone (window::DaemonRowButtonForm::Icons). A
    /// words-only button (Remove) keeps its words whatever the room.
    property bool iconWanted: false

    /// `{ words, label, icon, glyph, command }` of the button on this row.
    readonly property var face: chromeStyle.daemonRowButton(root.button, root.pending)
    /// Whether the button has an icon-only form on this row.
    readonly property bool hasIcon: (root.face.icon ?? "") !== "" || (root.face.glyph ?? "") !== ""
    /// Whether it shows its icon alone now.
    readonly property bool iconOnly: root.iconWanted && root.hasIcon
    /// The width the button takes with its words, whatever it shows now: what the row's fit is asked with.
    readonly property real textExtent: chromeStyle.snapToCells(wordsMetrics.advanceWidth
                                                               + chromeStyle.space.s3 + chromeStyle.space.s3)
    /// The width the button takes in its icon form: the icon and a small step either side, no wider than a
    /// toolbar button, so two of them leave a narrow sidebar's row room for the name; on the Terminal row one
    /// cell, as its glyph is a one-cell mark (window::DaemonRowButtonFaces); its words for a words-only button.
    readonly property real iconExtent: !root.hasIcon ? root.textExtent
        : root.cellTree ? chromeStyle.cellWidth
        : chromeStyle.snapToCells(Math.min(root.extent, root.iconSize + chromeStyle.space.s1 + chromeStyle.space.s1))
    readonly property bool cellTree: chromeStyle.sidebar.look === chromeStyle.sidebar.cellTree

    objectName: "sidebarDaemonButton"
    accessibleName: root.face.words ?? ""
    toolTipCommand: root.face.command ?? ""
    glyph: root.iconOnly ? (root.face.glyph ?? "") : (root.face.label ?? "")
    iconSource: root.iconOnly ? (root.face.icon ?? "") : ""
    // The Terminal row's glyph is set in its cells' face, as its words are.
    glyphFont: root.iconOnly && !root.cellTree ? chromeStyle.newTabFont : chromeStyle.type.footnoteStrong
    implicitWidth: root.iconOnly ? root.iconExtent : root.textExtent

    TextMetrics {
        id: wordsMetrics
        font: chromeStyle.type.footnoteStrong
        text: root.face.label ?? ""
    }
}
