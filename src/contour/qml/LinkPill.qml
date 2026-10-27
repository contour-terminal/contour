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
    /// Which of the pill's places the pane's find bar holds (a window::FindBarPresence, from
    /// chromeStyle.findBarPresence()): the pill and the bar never overlap.
    property int findBar: chromeStyle.findBarPresence("")

    // Guarded against `undefined`, not merely against a null session: a mock session may carry no link target
    // at all, and reading one into a typed property would be a QML warning.
    readonly property bool hasTarget: session !== null && session !== undefined
                                      && session.linkTargetText !== undefined
    readonly property bool wanted: hasTarget && session.linkTargetShown
    readonly property bool inHeader: placement.anchor === "headerTrailing" && headerSlot !== null
    // Kept in a header that has since gone (a pane rebinding mid-fade): the slot's coordinates mean nothing
    // over the grid, so the pill shows nowhere rather than jump there.
    readonly property bool slotGone: placement.anchor === "headerTrailing" && headerSlot === null
    /// The narrowest the pill is cut to: its hint, its insets, and one pill-height of the target, the rest of
    /// which is elided in its middle. A header slot narrower than this sends the pill to the grid.
    readonly property real minimumWidth: restImplicitWidth + extent
    /// Its whole width, the target uncut: known before the pill has ever been laid out.
    readonly property real naturalWidth: restImplicitWidth + textImplicitWidth

    // Where the pill would go now. Placed by its whole width (naturalWidth, the uncut text's), so cutting
    // the text cannot move it. Re-read as the clearance's revision moves: the page under a shown pill changed.
    readonly property var livePlacement: {
        if (!pill.wanted || !pill.session || pill.gridItem === null)
            return { anchor: "hidden", x: 0, y: 0, width: 0 }
        var revision = pill.session.overlayClearanceRevision
        var host = pill.headerPresent > 0 && pill.headerSlot !== null ? 1 : 0
        return chromeStyle.linkPillPlacement(host, pill.findBar, host > 0 ? pill.headerSlot.width : 0,
                                             pill.gridItem.width, pill.gridItem.height, pill.naturalWidth,
                                             pill.minimumWidth, pill.extent, pill.session.overlayClearance())
    }

    // Where the pill is drawn: the live placement while it shows, and the last shown one while it fades out,
    // so a hiding pill fades where it stood rather than at the grid's corner, at no width, over the
    // application's cells. Taken up again once it is fully faded.
    property var placement: ({ anchor: "hidden", x: 0, y: 0, width: 0 })
    function followPlacement() {
        if (pill.livePlacement.anchor !== "hidden" || pill.opacity === 0)
            pill.placement = pill.livePlacement
    }
    onLivePlacementChanged: pill.followPlacement()
    onOpacityChanged: pill.followPlacement()
    Component.onCompleted: pill.followPlacement()

    parent: pill.inHeader ? pill.headerSlot : pill.gridHost
    x: pill.placement.x
    y: pill.inHeader ? (parent.height - height) / 2 : pill.placement.y
    width: pill.placement.width
    z: 20

    extent: chromeStyle.size.linkPill
    cornerRadius: chromeStyle.radii.linkPill
    inset: chromeStyle.space.s5
    spacing: chromeStyle.space.s3
    textElide: Text.ElideMiddle
    elevation: chromeStyle.elevation.inPane
    // Opaque on a translucent profile too (ruling A5): the pill sits outside the pane's opacity, and its
    // legibility comes before matching the pane's translucency.
    text: pill.hasTarget ? pill.session.linkTargetText : ""
    detail: pill.hasTarget ? pill.session.linkTargetHint : ""
    shown: pill.wanted && pill.livePlacement.anchor !== "hidden"
    // Present only while it is (or is fading) on screen, so a hidden pill takes no press and is no
    // accessible object.
    visible: opacity > 0 && !slotGone

    Accessible.role: Accessible.ToolTip
    Accessible.name: pill.detail.length > 0 ? pill.text + qsTr(", ") + pill.detail : pill.text
}
