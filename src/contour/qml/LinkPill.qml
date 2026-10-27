// vim:syntax=qml
// The link pill: while the follow modifier is held over a link (or on plain hover, where
// hyperlink_hover_tooltip asks for it), the link's target and how to open it, in a Toast pill. It sits in
// the pane header's trailing area where the pane has an inline header, else at the top right of the grid,
// else at its bottom right -- and only ever over blank cells off the prompt and cursor rows; where no place
// is blank it shows nowhere (window::placeLinkPill, through chromeStyle.linkPillPlacement). The session
// decides what it says and whether it is wanted (linkTargetText, linkTargetHint, linkTargetShown) and says
// where the grid may be covered (overlayClearance(), re-read as overlayClearanceRevision moves).
//
// The target is an application's choice (an OSC 8 URI): Toast shows both texts as plain text, never as
// markup, and a long one is cut in its middle to the width the placement gives it.
import QtQuick
import QtQuick.Controls

Toast {
    id: pill
    objectName: "linkPill"

    /// The TerminalSession whose link this names; null while a pane rebinds.
    property var session: null
    /// The grid area the placement is measured against: the pane's chrome.
    property Item gridItem: null
    /// Where the pill sits over the grid: an item laid exactly over gridItem, above the pane's dim.
    property Item gridHost: gridItem
    /// The pane header's trailing slot, where the pane has an inline header; null where it has none.
    property Item headerSlot: null
    /// Whether the pane shows an inline header (1) or not (0): an overlay header covers cells, so it is none.
    property int headerPresent: 0

    // Guarded against `undefined`, not merely against a null session: a mock session may carry no link target
    // at all, and reading one into a typed property would be a QML warning.
    readonly property bool hasTarget: session !== null && session !== undefined
                                      && session.linkTargetText !== undefined
    readonly property bool wanted: hasTarget && session.linkTargetShown
    readonly property bool inHeader: placement.anchor === "headerTrailing"

    // Placed by its whole width (implicitWidth is the uncut text's), so cutting the text cannot move it. Re-read
    // as the clearance's revision moves: the page under a shown pill changed.
    readonly property var placement: {
        if (!pill.wanted || pill.gridItem === null)
            return { anchor: "hidden", x: 0, y: 0, width: 0 }
        var revision = pill.session.overlayClearanceRevision
        var host = pill.headerPresent > 0 && pill.headerSlot !== null ? 1 : 0
        // The find bar's input stays "closed" (0) until the find bar is on the branch (Step 3b).
        return chromeStyle.linkPillPlacement(host, 0, pill.gridItem.width, pill.gridItem.height,
                                             pill.implicitWidth, pill.extent, pill.session.overlayClearance())
    }

    parent: pill.inHeader ? pill.headerSlot : pill.gridHost
    x: pill.inHeader ? parent.width - width : pill.placement.x
    y: pill.inHeader ? (parent.height - height) / 2 : pill.placement.y
    width: pill.inHeader ? Math.min(pill.implicitWidth, pill.headerSlot.width) : pill.placement.width
    z: 20

    extent: chromeStyle.size.linkPill
    cornerRadius: chromeStyle.radii.linkPill
    inset: chromeStyle.space.s5
    spacing: chromeStyle.space.s3
    textElide: Text.ElideMiddle
    elevation: chromeStyle.elevation.inPane
    text: pill.hasTarget ? pill.session.linkTargetText : ""
    detail: pill.hasTarget ? pill.session.linkTargetHint : ""
    shown: pill.wanted && pill.placement.anchor !== "hidden"
    // Present only while it is (or is fading) on screen, so a hidden pill takes no press and is no
    // accessible object.
    visible: opacity > 0

    Accessible.role: Accessible.ToolTip
    Accessible.name: pill.detail.length > 0 ? pill.text + qsTr(", ") + pill.detail : pill.text
}
