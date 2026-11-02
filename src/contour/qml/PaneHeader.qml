// vim:syntax=qml
// The kit's PaneHeader: a pane's program status, its name, its working directory and four action buttons
// (split right, split down, zoom, close), on a pane card once the tab is split. PaneCard.qml decides whether
// and where it shows; this draws what the provider's decision (`policy`, chromeStyle.paneHeader) says:
// the working directory only on a pane wide enough, the actions on hover (or as the row's rule says for a
// focused pane), the text's emphasis by focus.
//
// The buttons are the pointer's way to the pane actions, and take Tab focus while shown; a button that
// hides (the actions fade, or the header goes) with the focus on it hands the focus back through
// focusReleased(), since Qt only drops it. The keyboard's way is the actions' own key bindings, which every
// button runs: SplitVertical (split right), SplitHorizontal (split down), TogglePaneZoom and ClosePane
// (window::PaneHeaderActions).
//
// While the program in the pane hides what is typed (a password prompt) the header shows the lock after the
// status, and `<process> · Secure input on` (`Input hidden` where the OS has no secure keyboard entry) in
// place of the name: a header is shown, so the lock goes here (window::SecureIndicatorRules).
//
// While the pane's sticky command header lives here (session.stickyHeader, set by the card through
// setStickyHeaderHost) it takes the name's place: `✓ cmake --build · 2m 14s`, the glyph inked by its status
// role, the command eliding in the middle first, and a link that jumps to the command. The working directory
// gives way to it; the name is back as soon as there is no header to show.
//
// The name is never elided before the working directory, which elides in the middle so that both the
// root and the leaf of a long path survive; assistive technology always hears the whole path.
//
// The header is also the pane's handle: dragging it picks the pane up (a DragHandler driving an automatic
// Drag, as a tab's is) and carries it, as a DragCard miniature, to whatever takes `application/x-contour-node`
// -- another pane's edge or middle, a tab, a gap of a tab strip, or the desktop. The buttons keep their
// clicks. While the pane is carried, `dragging` is true and its card shows its place empty.
//
// On the native rows the header is the card's own background (the row's header fill is transparent); on
// the Terminal row it is one cell row of the bar.
import QtQuick
import QtQuick.Window

Item {
    id: header
    objectName: "paneHeader"

    /// The TerminalSession of the pane this heads; null while a split rebinds.
    required property var session
    /// The PaneProxy whose actions the buttons run; null while a split rebinds.
    required property var node
    /// The provider's decision: `{ content, actions, placement, emphasis }` as their names.
    property var policy: ({})
    /// The card's corner radius: the header's top corners take it, its bottom edge stays square.
    property real cornerRadius: 0
    /// What holds the header's trailing area, one of chromeStyle.headerOccupants
    /// (window::HeaderTrailingOccupant): none, the find bar while it has taken this row (the Terminal row's,
    /// SearchBar's headerRow anchor), or the link pill while it shows there. Whatever holds it, the name stays,
    /// and the working directory and the actions give it their room.
    property int trailingOccupant: chromeStyle.headerOccupants.none
    readonly property bool trailingHeld: trailingOccupant !== chromeStyle.headerOccupants.none
    // Where the find bar goes when it takes this row: after the name, to the trailing edge.
    readonly property alias findSlot: findSlotItem
    /// Where the link pill sits in the header: from after the pane's name to the trailing edge, full height.
    readonly property alias linkPillSlot: linkPillSlotItem

    /// The pane's own background, which every colour here is resolved against.
    readonly property color background: session ? session.backgroundColor : "black"
    readonly property bool isOverlay: policy.placement === "overlay"
    readonly property string name: chromeStyle.paneHeaderName(session ? session.foregroundProcessName : "",
                                                               session ? session.title : "")
    readonly property string workingDirectory: session ? session.displayWorkingDirectory : ""
    /// The sticky command header, `{ glyph, command, detail, statusRole, jumpLabel, ... }`; empty while the
    /// pane's header is not where it lives, or there is nothing to show.
    readonly property var sticky: session && session.stickyHeader !== undefined ? session.stickyHeader : ({})
    readonly property bool stickyShown: sticky.command !== undefined && sticky.command !== ""
    /// What the sticky text reads: `✓ cmake --build · 2m 14s`.
    readonly property string stickyText: stickyShown
        ? sticky.glyph + " " + sticky.command + (sticky.detail !== "" ? qsTr(" · ") + sticky.detail : "")
        : ""
    /// What a screen reader says for it: the sticky text without its duration, which ticks every second while
    /// the block runs. Announced when the command or its outcome's glyph changes, not at every tick.
    readonly property string stickyName: stickyShown ? sticky.glyph + " " + sticky.command : ""
    /// What the sticky header's glyph and jump link are drawn on: the pane's background under the header's fill.
    readonly property color stickyGround: chromeStyle.paletteRevision < 0 ? "transparent"
        : Qt.tint(background, chromeStyle.colorOn(chromeStyle.roles.paneHeaderFill, background))
    /// Whether the pane is at a password prompt, and the rule puts the lock in a shown header.
    readonly property bool locked: !!session && (session.secureIndicator ?? 0) >= chromeStyle.secureInputs.locked
        && chromeStyle.secureIndicatorSpots(chromeStyle.secureInputs.spotShown,
                                            chromeStyle.secureInputs.spotHidden,
                                            chromeStyle.secureInputs.spotHidden).header === true
    /// What the name slot reads: the name, or while locked the name and the lock's state.
    // Concatenated, not formatted: a title the application set may itself contain "%2".
    readonly property string shownName: locked ? name + qsTr(" · ") + chromeStyle.secureInputText(session.secureIndicator) : name
    readonly property int programStatus: session ? session.programStatus : chromeStyle.badgeKinds.none
    // colorOn() and badgeLook() read the palette, hence the paletteRevision read in each binding.
    readonly property var statusLook: chromeStyle.paletteRevision < 0 ? ({})
                                                                     : chromeStyle.badgeLook(programStatus)
    readonly property color dimmedText: chromeStyle.paletteRevision < 0 ? "transparent"
        : chromeStyle.colorOn(chromeStyle.roles.paneHeaderTextDimmed, background)
    readonly property color text: chromeStyle.paletteRevision < 0 ? "transparent"
        : policy.emphasis === "primary" ? chromeStyle.colorOn(chromeStyle.roles.paneHeaderText, background)
        : dimmedText

    // The layout's three spacings: the kit's leading and trailing padding, and the gap between items.
    readonly property real leading: chromeStyle.space.s5
    readonly property real trailing: chromeStyle.space.s3
    readonly property real gap: chromeStyle.space.paneHeaderGap

    /// A header button held the keyboard focus and has hidden, leaving it nowhere: the card's terminal takes it.
    signal focusReleased()

    /// The window's controller (Main.qml's `win`); null while the window is torn down.
    readonly property var controller: Window.window && Window.window.win ? Window.window.win : null
    /// The pixmap the OS drag cursor shows (a grab of the DragCard); empty until the grab lands.
    property url dragImage: ""

    // The grab is ASYNCHRONOUS, so the card is grabbed as the header is pressed, ahead of the drag that press
    // may start: once per drag, with the lines the pane shows then. With no image ready the drag still works.
    function grabDragImage() {
        // grabToImage warns unless the item is realized: a size, and a visible window.
        if (dragCard.width <= 0 || dragCard.height <= 0 || !header.Window.window || !header.Window.window.visible)
            return
        // What the pane shows now, for the card's body.
        dragCard.lines = header.session && header.session.visibleTextTail ? header.session.visibleTextTail(3) : []
        dragCard.grabToImage(function(result) { header.dragImage = result.url })
    }

    // Whether one of the buttons took the keyboard focus, and it has not moved on to something else since.
    property bool actionHeldFocus: false

    // Whether item is ancestor or lies within it.
    function isWithin(item, ancestor) {
        for (var each = item; each; each = each.parent)
            if (each === ancestor)
                return true
        return false
    }

    // Settles where a button's focus went, once the hiding (or move) that took it has run its course: still
    // on a shown button, moved on to another item, or dropped by a hide -- nothing holds it, a hidden item
    // keeps it, or a scope above the header was left holding it -- which is the one case that hands it back.
    function settleFocus() {
        if (!actionHeldFocus)
            return
        var item = Window.activeFocusItem
        var shown = header.visible && actions.visible
        if (shown && item !== null && isWithin(item, actions))
            return
        actionHeldFocus = false
        var dropped = item === null || !item.visible || (item !== header && isWithin(header, item))
        if (dropped && !shown)
            header.focusReleased()
    }

    onVisibleChanged: Qt.callLater(header.settleFocus)

    Accessible.role: Accessible.StaticText
    // Concatenated, not formatted: a title the application set may itself contain "%2".
    Accessible.name: stickyShown ? stickyName
                   : workingDirectory !== "" ? shownName + qsTr(", ") + workingDirectory : shownName

    // A click on the header, as on the terminal, makes the pane the active one. It takes the press for
    // itself: an overlay header lies over the terminal, which must not see the click as its own.
    TapHandler {
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: if (header.node) header.node.activate()
    }

    // The card the OS drag shows. Never visible in the scene: grabToImage renders it all the same.
    DragCard {
        id: dragCard
        visible: false
        name: header.shownName
        background: header.background
        mark: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.accentColor
        ink: header.dimmedText
        lineFamily: header.session && header.session.fontFamily ? header.session.fontFamily : "monospace"
    }

    // The drag's carrier; the pane itself stays put, and the OS cursor shows the card.
    NodeDragSource {
        id: paneDragProxy
        objectName: "paneDragProxy"
        controller: header.controller
        begin: function() { return header.session ? header.controller.beginPaneDrag(header.session) : "" }
        image: header.dragImage
        hotSpot: dragCard.hotSpot
    }

    DragHandler {
        id: paneDragHandler
        objectName: "paneDragHandler"
        target: null
        acceptedButtons: Qt.LeftButton
        // As a tab's (TabItem.qml): never takes a gesture from an item that already holds it, so a header
        // button keeps its press and the window's resize handles keep theirs.
        grabPermissions: PointerHandler.CanTakeOverFromHandlersOfDifferentType
                         | PointerHandler.ApprovesTakeOverByHandlersOfDifferentType
                         | PointerHandler.ApprovesTakeOverByItems
                         | PointerHandler.ApprovesCancellation
        // A small threshold so a click still activates the pane; only a real drag picks it up.
        dragThreshold: 8
        onActiveChanged: if (active) paneDragProxy.carry()
        // A press, which every drag begins with: the card is grabbed now, so its image is ready when it is.
        onGrabChanged: (transition) => { if (transition === PointerDevice.GrabPassive) header.grabDragImage() }
    }

    // The fill: the card's background (opaque as an overlay, at the profile's opacity inline, as the
    // terminal below it is) under the row's header fill. The rounded fill reaches one radius below the
    // header and is clipped there, so the top corners follow the card and the bottom edge meets the
    // terminal square.
    Item {
        anchors.fill: parent
        clip: header.cornerRadius > 0

        Rectangle {
            objectName: "paneHeaderBackground"
            anchors.fill: parent
            anchors.bottomMargin: -header.cornerRadius
            radius: header.cornerRadius
            color: header.background
            opacity: header.isOverlay || !header.session ? 1.0 : header.session.opacity
        }

        Rectangle {
            objectName: "paneHeaderFill"
            anchors.fill: parent
            anchors.bottomMargin: -header.cornerRadius
            radius: header.cornerRadius
            color: chromeStyle.paletteRevision < 0 ? "transparent"
                 : chromeStyle.colorOn(chromeStyle.roles.paneHeaderFill, header.background)
        }
    }

    Text {
        id: status
        objectName: "paneHeaderStatus"
        visible: header.programStatus !== chromeStyle.badgeKinds.none && text !== ""
        x: header.leading
        anchors.verticalCenter: parent.verticalCenter
        text: header.statusLook.glyph !== undefined ? header.statusLook.glyph : ""
        font: chromeStyle.type.paneHeader
        color: header.statusLook.ink !== undefined ? header.statusLook.ink : header.dimmedText
        Accessible.ignored: true
    }

    SecureInputLock {
        id: lock
        objectName: "paneHeaderLock"
        visible: header.locked
        indicator: header.session ? (header.session.secureIndicator ?? chromeStyle.secureInputs.locked) : chromeStyle.secureInputs.locked
        x: status.visible ? status.x + status.width + header.gap : header.leading
        anchors.verticalCenter: parent.verticalCenter
        // The header's secondary label, whatever the focus: the canvas's label.secondary.
        color: header.dimmedText
        font: chromeStyle.type.footnote
    }

    Text {
        id: nameLabel
        objectName: "paneHeaderName"
        x: lock.visible ? lock.x + lock.width + header.gap
         : status.visible ? status.x + status.width + header.gap : header.leading
        anchors.verticalCenter: parent.verticalCenter
        // Its whole width first: only a header too narrow for the name alone cuts it.
        // While the sticky header is shown it holds this slot instead, and the name is not drawn.
        width: header.stickyShown ? stickyItem.width
                                  : Math.min(implicitWidth, Math.max(0, actions.x - header.gap - x))
        visible: !header.stickyShown
        text: header.shownName
        elide: Text.ElideRight
        font: chromeStyle.type.paneHeader
        color: header.text
        Accessible.ignored: true
    }

    Text {
        id: cwdLabel
        objectName: "paneHeaderCwd"
        visible: !header.trailingHeld && !header.stickyShown && header.policy.content === "nameAndCwd" && width > 0
        x: nameLabel.x + nameLabel.width + header.gap
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, actions.x - header.gap - x)
        text: header.workingDirectory
        elide: Text.ElideMiddle
        // The footnote face: the header's size on every row, at the regular weight the name does not take.
        font: chromeStyle.type.footnote
        color: header.dimmedText
        Accessible.ignored: true
    }

    // The sticky command header: its glyph, its command and its detail, in the name's slot.
    Item {
        id: stickyItem
        objectName: "paneHeaderSticky"
        visible: header.stickyShown
        x: nameLabel.x
        anchors.verticalCenter: parent.verticalCenter
        height: stickyCommand.height
        // The command gives way first; the glyph and the detail are short and keep their room.
        width: Math.min(stickyGlyph.width + header.gap + stickyCommand.implicitWidth
                        + (stickyDetail.visible ? header.gap + stickyDetail.width : 0),
                        Math.max(0, actions.x - header.gap - x))
        /// What it reads, for the card's checks and assistive technology.
        readonly property string text: header.stickyText
        Accessible.ignored: true

        Text {
            id: stickyGlyph
            objectName: "paneHeaderStickyGlyph"
            anchors.verticalCenter: parent.verticalCenter
            text: header.stickyShown ? header.sticky.glyph : ""
            font: chromeStyle.type.paneHeader
            color: chromeStyle.paletteRevision < 0 || !header.stickyShown ? "transparent"
                 : chromeStyle.statusMarkOn(header.sticky.statusRole, header.stickyGround)
        }

        Text {
            id: stickyCommand
            objectName: "paneHeaderStickyCommand"
            x: stickyGlyph.width + header.gap
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, stickyItem.width - x - (stickyDetail.visible ? header.gap + stickyDetail.width : 0))
            text: header.stickyShown ? header.sticky.command : ""
            elide: Text.ElideMiddle
            font: chromeStyle.type.paneHeader
            color: header.text
        }

        Text {
            id: stickyDetail
            objectName: "paneHeaderStickyDetail"
            visible: header.stickyShown && header.sticky.detail !== ""
            x: stickyItem.width - width
            anchors.verticalCenter: parent.verticalCenter
            text: header.stickyShown ? qsTr("· ") + header.sticky.detail : ""
            font: chromeStyle.type.footnote
            color: header.dimmedText
        }
    }

    // The link that brings the sticky header's command to the top of the pane, in the accent.
    Text {
        id: jumpLink
        objectName: "paneHeaderJump"
        visible: header.stickyShown && !header.trailingHeld && width > 0
        x: stickyItem.x + stickyItem.width + header.gap
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, Math.max(0, actions.x - header.gap - x))
        text: header.stickyShown ? header.sticky.jumpLabel : ""
        elide: Text.ElideRight
        font: chromeStyle.type.footnote
        color: chromeStyle.paletteRevision < 0 ? "transparent"
             : chromeStyle.statusTextOn(chromeStyle.statusRunning, header.stickyGround)
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.onPressAction: if (header.session) header.session.jumpToStickyHeader()

        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: if (header.session) header.session.jumpToStickyHeader()
        }
        HoverHandler { cursorShape: Qt.PointingHandCursor }
    }

    Item {
        id: findSlotItem
        objectName: "paneHeaderFindSlot"
        x: cwdLabel.x // Where the directory starts: after the name.
        width: Math.max(0, header.width - header.trailing - x)
        height: header.height
    }

    Row {
        id: actions
        objectName: "paneHeaderActions"
        x: header.width - header.trailing - width
        anchors.verticalCenter: parent.verticalCenter
        spacing: header.gap
        // One fade, on every row: under Reduce Motion motion.fast is already the cross-fade's duration.
        opacity: header.policy.actions === "shown" ? 1.0 : 0.0
        visible: !header.trailingHeld && opacity > 0
        onVisibleChanged: Qt.callLater(header.settleFocus)
        Behavior on opacity { NumberAnimation { duration: chromeStyle.motion.fast } }

        Repeater {
            model: chromeStyle.paneActionGlyphs
            delegate: ToolbarButton {
                required property int index
                required property string modelData
                objectName: "paneHeaderAction" + index
                extent: chromeStyle.size.paneHeaderAction
                activeFocusOnTab: true
                glyph: modelData
                glyphFont: chromeStyle.type.paneHeaderAction
                glyphColor: header.text
                // By the pane's own tone, as the text is: the band's hover fill follows the chrome's.
                hoverFill: chromeStyle.paletteRevision < 0 ? "transparent"
                         : chromeStyle.colorOn(chromeStyle.roles.paneHeaderActionHover, header.background)
                accessibleName: chromeStyle.paneHeaderActionNames[index]
                toolTipCommand: chromeStyle.paneHeaderActionCommands[index]
                onClicked: if (header.node) header.node.runHeaderAction(index)
                onActiveFocusChanged: {
                    if (activeFocus)
                        header.actionHeldFocus = true
                    else
                        Qt.callLater(header.settleFocus)
                }
            }
        }
    }

    Item {
        id: linkPillSlotItem
        objectName: "paneHeaderLinkSlot"
        x: nameLabel.x + nameLabel.width + header.gap
        y: 0
        width: Math.max(0, header.width - header.trailing - x)
        height: header.height
    }
}
