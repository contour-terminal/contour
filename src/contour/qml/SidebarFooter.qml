// vim:syntax=qml
// The sidebar's footer: the "+" that makes a workspace where the row keeps it here (a source list's, a
// navigation pane's, the cell tree's: chromeStyle.sidebar.addInFooter), what the panes the window lists ask
// for (SidebarModel.footerText: the counts that are not zero, in the user's language -- a filter hides none
// of them), and the chord that jumps to the next one that asks for the user. The counts are what the footer
// is for: the chord is dropped when both do not fit whole (SidebarModel.footerChord), and where the full
// wording does not fit alone the counts take a shorter one (SidebarModel.footerCounts) -- fewer letters, then
// the rows' status glyphs -- whose full wording stays their accessible name and tooltip.
import QtQuick
import Contour.Terminal

Item {
    id: root
    objectName: "sidebarFooter"

    /// The sidebar's model.
    required property var sidebar
    /// The window's controller, which names the jump's chord and makes a workspace.
    required property var controller

    readonly property string fullCounts: root.sidebar ? root.sidebar.footerText : ""
    readonly property string compactCounts: root.sidebar ? root.sidebar.footerCompactText : ""
    readonly property string glyphCounts: root.sidebar
        ? chromeStyle.sidebarFooterGlyphText(root.sidebar.footerNeedYou, root.sidebar.footerWorking) : ""
    /// The wording the counts take: the longest that fits alone, a pure decision of the model's.
    readonly property int wording: root.sidebar === null || root.sidebar === undefined
        ? SidebarRows.FooterCounts.Full
        : root.sidebar.footerCounts(root.width - root.countsStart - root.inset, fullMetrics.advanceWidth,
                                    compactMetrics.advanceWidth)
    readonly property string counts: root.wording === SidebarRows.FooterCounts.Full ? root.fullCounts
                                   : root.wording === SidebarRows.FooterCounts.Compact ? root.compactCounts
                                   : root.glyphCounts
    readonly property string jumpChord: root.controller && root.controller.shortcutText
                                        ? root.controller.shortcutText("JumpToNextAttention") : ""
    // The footer's ends: a cell on the Terminal row, whose small spacing steps are no cells at all.
    readonly property real inset: chromeStyle.sidebar.dividerGlyph !== ""
                                  ? Math.max(chromeStyle.space.s3, chromeStyle.cellWidth) : chromeStyle.space.s3

    // Where the counts start, the gap the chord keeps from them, and the room both share.
    readonly property real countsStart: addButton.visible ? addButton.x + addButton.width + root.inset
                                                          : chromeStyle.space.s4
    readonly property real chordGap: chromeStyle.sidebar.dividerGlyph !== ""
                                     ? Math.max(chromeStyle.space.s2, chromeStyle.cellWidth) : chromeStyle.space.s2
    /// Whether the chord has room beside the whole counts: a pure decision of the model's.
    readonly property bool chordFits: root.sidebar !== null && root.sidebar !== undefined
        && root.sidebar.footerChord(root.width - root.countsStart - root.inset, fullMetrics.advanceWidth,
                                    chord.implicitWidth, root.chordGap) === SidebarRows.FooterChord.Shown

    TextMetrics { id: fullMetrics; font: chromeStyle.type.footnote; text: root.fullCounts }
    TextMetrics { id: compactMetrics; font: chromeStyle.type.footnote; text: root.compactCounts }

    implicitHeight: chromeStyle.sidebar.footerHeight

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: chromeStyle.sidebar.look === chromeStyle.sidebar.cellTree ? 0 : chromeStyle.borderWidth
        color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarDivider
    }

    SidebarControlButton {
        id: addButton
        objectName: "sidebarFooterNewWorkspaceButton"
        control: "newWorkspace"
        visible: chromeStyle.sidebar.addInFooter
        anchors.left: parent.left
        anchors.leftMargin: root.inset
        anchors.verticalCenter: parent.verticalCenter
        extent: Math.min(chromeStyle.size.toolbarButton, root.height)
        implicitWidth: chromeStyle.snapToCells(extent)
        glyphColor: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarSecondaryInk
        accessibleName: qsTr("New workspace")
        dropSidebar: root.sidebar
        onClicked: if (root.controller && root.controller.newWorkspace) root.controller.newWorkspace()
    }

    Text {
        id: countsText
        objectName: "sidebarFooterCounts"
        anchors.left: parent.left
        anchors.leftMargin: root.countsStart
        anchors.right: chord.visible ? chord.left : parent.right
        anchors.rightMargin: chord.visible ? root.chordGap : root.inset
        anchors.verticalCenter: parent.verticalCenter
        visible: text !== ""
        textFormat: Text.PlainText
        text: root.counts
        font: chromeStyle.type.footnote
        color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarSecondaryInk
        elide: Text.ElideRight
        Accessible.role: Accessible.StaticText
        Accessible.name: root.fullCounts

        // A shorter wording says in full what it counts where the pointer rests on it.
        KitToolTipHost {
            objectName: "sidebarFooterCountsToolTipHost"
            text: countsText.text !== root.fullCounts ? root.fullCounts : ""
            toolTipName: "sidebarFooterCountsToolTip"
        }
    }

    Text {
        id: chord
        objectName: "sidebarFooterChord"
        anchors.right: parent.right
        anchors.rightMargin: root.inset
        anchors.verticalCenter: parent.verticalCenter
        visible: text !== "" && root.chordFits
        textFormat: Text.PlainText
        text: root.jumpChord
        font: chromeStyle.type.footnote
        color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.sidebarSecondaryInk
        Accessible.ignored: true
    }
}
