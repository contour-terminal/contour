// vim:syntax=qml
// How a picker's row wears the selection: the platform's list convention, the one the sidebar follows
// (window::sidebarSelectionPaint, through chromeStyle.sidebarSelection). A picker's list always has the
// keyboard while it is open, so its selection is the focused list's: the accent fill on macOS, a neutral fill
// on Linux, a grey fill with the accent pill on Fluent, inverse video on the Terminal row. The keyboard itself
// is in the search field, not on the row, so no row draws the focus ring (ui-ux rule 6.1): the fill alone
// marks the selection. One rule for every list (ui-ux rule 1.4); this item only draws what it is told, and hands the
// row the ink to draw its text in.
//
// Fill it over the row's selectable area, at the palette's inset.
import QtQuick
import Contour.Terminal

Item {
    id: root

    /// Whether the row is the selected one.
    property bool selected: false
    /// The fill's corner radius.
    property real radius: chromeStyle.radii.control
    /// The prefix of the fill's and the pill's objectName (`<prefix>Selection`, `<prefix>AccentPill`).
    property string namePrefix: "picker"

    /// What the look decides for the selected row of a list that has the keyboard, the keyboard being in the
    /// field rather than on the row: `{ fill, ring, pill }`, the ring never drawn.
    readonly property var paint: chromeStyle.sidebarSelection(SidebarRows.RowSelection.Selected, true, false)
    readonly property bool emphasized: root.paint.fill === "emphasized"
    /// The row's primary text colour.
    readonly property color ink: !root.selected ? chromeStyle.colors.paletteText
                                 : root.emphasized ? chromeStyle.colors.sidebarSelectionInk
                                 : chromeStyle.colors.sidebarSelectionInkInactive
    /// The row's secondary text colour (a shortcut, a detail, a glyph).
    readonly property color secondaryInk: !root.selected ? chromeStyle.colors.paletteSecondaryText : root.ink

    Rectangle {
        objectName: root.namePrefix + "Selection"
        anchors.fill: parent
        visible: root.selected
        radius: root.radius
        color: root.emphasized ? chromeStyle.colors.sidebarSelectionFill
                               : chromeStyle.colors.sidebarSelectionFillInactive

        Rectangle {
            objectName: root.namePrefix + "AccentPill"
            visible: chromeStyle.sidebar.accentPillWidth > 0 && root.paint.pill
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: chromeStyle.sidebar.accentPillWidth
            height: chromeStyle.sidebar.glyph
            radius: chromeStyle.sidebar.accentPillWidth
            color: chromeStyle.colors.sidebarAccentPill
        }
    }

    FocusRing {
        shown: root.selected && root.paint.ring
        hostRadius: root.radius
    }
}
