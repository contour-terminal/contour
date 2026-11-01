// vim:syntax=qml
// One tab in the title-bar tab strip, in one of three shapes (chromeStyle.layout.tabShape):
//
//   - Pill (macOS): a rounded pill in the strip's well -- an icon square, the title, the badge slot and a
//     close button shown on hover and on the active tab. A coloured tab tints the pill and fills the icon.
//   - Joined (Windows): the same parts on a tab whose top corners are rounded and whose bottom meets the
//     content -- the active tab is the content's colour (R-k), the rest transparent over the band, each
//     ended by a rule. A coloured tab is filled as on the Flat strip.
//   - Flat (today's strip): the label, the badge slot and the close button on a fill. On the native rows
//     a coloured tab is filled with its colour (WT-style, faded when inactive/unfocused) and an uncoloured
//     one uses the OS highlight; on the Terminal row the fill is the colour roles' and the colour is a
//     leading "▌" cell.
//
// Single-click activates, double-click renames, right-click opens the context menu. Mutations go through
// `controller` (TerminalSessionManager, exposed as `terminalSessions`).
import QtQuick
import QtQuick.Controls
import QtQuick.Window
import Contour.Terminal

Item {
    id: root

    // A page tab, named by its title. Accessible.selected is not decoration: QtQuick's attached type
    // emits a state-change event from its setter, so binding it here IS the tab-switch announcement an
    // assistive client hears -- no explicit event needs raising anywhere.
    Accessible.role: Accessible.PageTab
    Accessible.name: root.tabTitle
    Accessible.selected: root.tabActive
    Accessible.focusable: true
    // Keyboard focus in the chrome moves a real active focus here (TabStrip.focusTab), so an assistive
    // client hears the focused tab, not only the selected one.
    Accessible.focused: root.keyboardFocused

    required property var controller
    required property var window          // ApplicationWindow, for restoring terminal focus after rename
    required property int tabIndex
    required property string tabTitle    // resolved/expanded label shown on the tab
    required property string tabRawTitle // un-expanded rename template, pre-fills the inline editor
    required property color tabColor
    required property bool tabActive
    required property int tabPaneCount
    required property bool tabZoomed     // active pane is zoomed: only it is on screen (see vtworkspace::Tab)
    // The active pane's OSC 9;4 progress, as vtbackend::ProgressState's value: 0 inactive (nothing
    // painted), 1 normal, 2 error, 3 indeterminate, 4 paused.
    required property int tabProgressState
    required property int tabProgressPercentage
    // The tab's badge slot (WindowController's BadgeRole): contour::window::TabBadgeKind's value, the
    // winner among its panes' OSC 7501 records, unseen finished commands and unseen output; the program's
    // disarmed tooltip text when its status won; and a working badge's progress (-1 for none). Not
    // required, so a TabItem built on its own shows no badge.
    property int tabBadge: 0
    property string tabBadgeText: ""
    property int tabBadgeProgress: -1
    // Whether any pane of the tab is at a password prompt (WindowController's SecureRole). A bool, the
    // QML-facing carve-out: the role says only yes or no. Not required, so a TabItem built on its own shows no
    // lock.
    property bool locked: false
    // What the lock says (a window::SecureIndicator's value); the model's SecureRole, the strongest of the
    // tab's panes.
    property int secureState: locked ? chromeStyle.secureInputs.locked : chromeStyle.secureInputs.none
    // The lock goes on the tab whenever the tab well shows it -- this tab is in it -- whatever the pane's
    // header shows (window::SecureIndicatorRules).
    readonly property bool lockShown: root.locked && !root.iconOnly
        && chromeStyle.secureIndicatorSpots(chromeStyle.secureInputs.spotHidden,
                                            chromeStyle.secureInputs.spotShown,
                                            chromeStyle.secureInputs.spotHidden).tab === true

    // What the pill shows, as TabStrip reads it from chromeStyle.tabWellLayout(): "full", or "iconOnly"
    // once the well is too narrow for labels. The Flat shape always shows its label.
    property string labelMode: "full"

    // Whether keyboard focus is on this tab: it holds active focus and the controller says the tab well has
    // it (focusedTabIndex is -1 outside the tab well, so no area is compared). A click never sets either,
    // so a pointer-activated tab shows no ring. `?? -1` keeps a mock controller without the property local.
    readonly property bool keyboardFocused: root.activeFocus && !!root.controller
                                            && (root.controller.focusedTabIndex ?? -1) === root.tabIndex

    // Which of the drawings this tab is (the style's QML-facing shape flags).
    readonly property bool pill: chromeStyle.layout.isPill
    readonly property bool joined: chromeStyle.layout.isJoined
    // Whether the tab is laid out as the kit draws it -- the icon square, the title, the badge, the close
    // button -- which the pill and the joined tab share, at their own insets.
    readonly property bool kitTab: root.pill || root.joined
    readonly property bool iconOnly: root.pill && root.labelMode === "iconOnly"
    // The kit tab's insets: before the icon, and between its parts (the kit's 6 on a pill, 8 joined).
    readonly property real leadingInset: root.joined ? chromeStyle.space.s6 : chromeStyle.space.s5
    readonly property real kitGap: root.joined ? chromeStyle.space.s4 : chromeStyle.space.s3
    // Whether the fill and the label come from the chrome colour roles rather than the OS palette: the
    // pills and the Terminal row do, so a scheme's legible selection pair is what the active tab wears.
    readonly property bool roleColors: chromeStyle.layout.tabColors === chromeStyle.layout.roles
    readonly property var colors: chromeStyle.colors

    readonly property bool hasBadge: badgeSlot.known
    // The close button shows on every Flat tab, and on a pill or a joined tab only while it is active or
    // hovered and has the room for one. A hidden button is hidden from assistive technology too, so an
    // inactive or an icon-only pill offers no "Close tab" there; closing it stays reachable through its
    // context menu and the keyboard (the CloseTab action on the tab once activated).
    readonly property bool closeShown: !root.kitTab || (!root.iconOnly && (root.tabActive || hover.hovered))
    // The gap between the parts of a tab: the kit's between a kit tab's icon, title, badge and close.
    readonly property real partGap: root.kitTab ? root.kitGap : chromeStyle.labelGap

    // The tab's natural size, in whatever unit the chrome style counts in: logical pixels natively,
    // whole character cells in the terminal style. `widthQuantum` is what expresses that difference
    // -- it is 1 natively, so the rounding below is a no-op and the result is the historical pixel
    // width, and one cell in the terminal style, where it snaps every tab onto the grid.
    //
    // The 56 that used to sit here was the label's leading inset, the gap after it and the close
    // button -- 10 + 4 + 22 -- plus 20 pixels of breathing room the literal never spelled out. All
    // four are tokens now, `tabSlack` being that leftover, so the sum says what it is made of AND
    // still comes to 56 natively: a tab is exactly as wide as it always was. `trailingPadding` is
    // part of that promise -- it is 0 natively, precisely so this sum is unchanged there.
    //
    // A pill is laid out as the kit draws it: s5, the icon, s3, the title, the badge and the close button
    // each after an s3, then s4. Its width is the well's (TabStrip assigns it); this is only what it would
    // like. A joined tab is the same at s6 and s4.
    readonly property real naturalWidth: root.kitTab
        ? root.leadingInset + icon.width + root.kitGap + lock.room + label.implicitWidth
          + root.partGap + badgeSlot.width + badgeSlot.gapAfter
          + zoomBadge.width + zoomBadge.anchors.rightMargin
          + chromeStyle.size.tabClose + chromeStyle.space.s4
        : lock.room + label.implicitWidth + chromeStyle.labelPadding
          + chromeStyle.labelGap + chromeStyle.controlWidth
          + chromeStyle.trailingPadding
          + separator.width + chromeStyle.tabSlack
          + zoomBadge.width + zoomBadge.anchors.rightMargin
          + badgeSlot.width + badgeSlot.gapAfter
          + colorCell.width
    // Snapped UP to the quantum (snapToCells): rounding to the nearest cell would take back up to half a
    // cell the label needs, and elide a title that fits.
    implicitWidth: chromeStyle.snapToCells(Math.min(chromeStyle.maxTabWidth,
                                                    Math.max(chromeStyle.minTabWidth, root.naturalWidth)))
    implicitHeight: root.kitTab ? chromeStyle.size.tab : chromeStyle.tabHeight

    // Live OS palette handle so the tab adapts to dark/light in realtime.
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Uncolored tabs report a transparent accentColor (WindowController ColorRole), so alpha is the flag.
    readonly property bool colored: root.tabColor.a > 0

    // Whether this is the strip's trailing tab, which is what decides if it draws a separator (see
    // the separator near the bottom of this file). Read from the view rather than passed in, so a
    // TabItem stays instantiable on its own: `ListView.view` is null outside a strip, which reads as
    // "there could be a tab after this one" and keeps the separator -- the conservative answer.
    readonly property bool lastInStrip: root.ListView.view
                                        ? root.tabIndex === root.ListView.view.count - 1
                                        : false

    // Subtle highlight wash shared by the uncolored-tab hover fill and the close button's hover, so the
    // hover tint (color + alpha) is defined once.
    readonly property color hoverWash: chromeStyle.wash(systemPalette.highlight)

    // A pill's own fill by state, before any tab colour: the well shows through a pill at rest.
    readonly property color pillStateFill: root.tabActive ? root.colors.tabActiveFill
                                         : hover.hovered ? root.colors.tabHoverFill
                                         : "transparent"
    // What a pill sits on: the band's ground with the well's fill over it. A tint is laid over this
    // rather than over the pill's translucent fill alone, so the tinted pill is opaque and its label's
    // contrast is the one measured against it.
    readonly property color wellGround: Qt.tint(systemPalette.window, root.colors.tabWellFill)

    // The content's colour (R-k), which a joined active tab fills with and its label is resolved on.
    readonly property color contentColor: root.colors.paneGapFill

    // The fill behind the label; also drives text contrast.
    //   - Pill: the state's fill, and a coloured tab's colour over it at chromeStyle.tint.tab.
    //   - Joined: the content's colour while active, the hover fill while hovered, else the band shows
    //     through; a coloured tab is filled as on the Flat strip below.
    //   - Flat with the colour roles (Terminal): the role fills; the colour is the leading cell instead.
    //   - Flat with the OS palette (today's native strip): a coloured tab's WT-style fade from
    //     TabColorScheme.hpp (via the controller), else OS highlight / hover wash / transparent.
    // controller is null-guarded (torn down before this tree on window close).
    readonly property color effectiveBackground: {
        if (root.pill) {
            if (!root.colored)
                return root.pillStateFill
            return Qt.tint(Qt.tint(root.wellGround, root.pillStateFill),
                           Qt.rgba(root.tabColor.r, root.tabColor.g, root.tabColor.b, chromeStyle.tint.tab))
        }
        if (root.joined && !root.colored)
            return root.tabActive ? root.contentColor : hover.hovered ? root.colors.tabHoverFill : "transparent"
        if (root.roleColors) {
            if (root.tabActive)
                return root.colors.tabActiveFill
            return hover.hovered ? root.colors.tabHoverFill : root.colors.tabWellFill
        }
        if (root.colored && root.controller)
            return root.controller.tabBackgroundColor(root.tabColor, systemPalette.window,
                                                      root.tabActive, hover.hovered, Window.active)
        if (root.tabActive)
            return systemPalette.highlight
        if (hover.hovered)
            return root.hoverWash
        return "transparent"
    }

    // Label / close-glyph color. With the colour roles: the active label, a tinted pill's label one level
    // up so it keeps 4.5:1 on the tint, else the inactive label -- each one level dimmer on a pill while
    // its window is in the background. With the OS palette: contrast against a coloured fill, else the
    // matching palette role.
    // A joined tab's labels are the kit's, the active one resolved on the content's colour it sits on, each
    // a level dimmer while the window is in the background (R-o).
    readonly property color foreground: {
        // A row that fills the focused item instead of ringing it (size.focusRing 0: the Terminal row)
        // inks the label for that fill, the selection's own text.
        if (root.keyboardFocused && chromeStyle.size.focusRing === 0)
            return root.colors.paletteSelectionText
        if (root.joined && !root.colored) {
            if (root.tabActive)
                return chromeStyle.colorOn(Window.active ? chromeStyle.roles.tabTextActive
                                                         : chromeStyle.roles.tabTextDimmed, root.contentColor)
            return Window.active ? root.colors.tabText : root.colors.tabTextDimmed
        }
        if (root.roleColors) {
            const background = root.pill && !Window.active
            if (root.tabActive)
                return background ? root.colors.tabText : root.colors.tabTextActive
            if (background)
                return root.colors.tabTextDimmed
            return root.colored && root.pill ? root.colors.tabTextTinted : root.colors.tabText
        }
        if (root.colored && root.controller)
            return root.controller.tabTextColor(root.effectiveBackground)
        return root.tabActive ? systemPalette.highlightedText : systemPalette.windowText
    }

    // The label's font: the kit's tab type on a pill, the chrome font on the Flat strip; for the active
    // tab, whatever the row sets it apart with (bold on the Terminal row, see activeTabFont).
    readonly property font baseLabelFont: root.kitTab ? chromeStyle.type.tab : chromeStyle.font
    readonly property font labelFont: root.tabActive ? chromeStyle.activeTabFont(root.baseLabelFont)
                                                     : root.baseLabelFont

    // The pixmap the OS drag cursor shows while this tab is dragged (see dragProxy). grabToImage is
    // ASYNCHRONOUS -- it delivers the url on a later frame -- so the tab grabs ahead of a drag: when the
    // pointer comes onto it or presses it, as every drag begins, and again on a change to its size, label,
    // color or active state while it is there. Not otherwise: a grab holds its image until the garbage
    // collector frees it, and one still in flight when the window or the engine goes away is never freed at
    // all. One grab is in flight at a time, and a change meanwhile asks for one more once it lands, so the
    // latest look wins. With no image ready the drag still works (Qt uses a default pixmap); only the ghost
    // is missing.
    enum DragImageGrab { Idle, InFlight, InFlightAndStale }
    property int dragImageGrab: TabItem.Idle
    property url dragImage: ""
    function refreshDragImage() {
        if (!hover.hovered && !leftTap.pressed)
            return
        // grabToImage warns (and, offscreen, fails the QML test gate) unless the item is realized: it
        // needs a non-zero size and a visible window.
        if (root.width <= 0 || root.height <= 0)
            return
        if (!root.Window.window || !root.Window.window.visible)
            return
        if (root.dragImageGrab !== TabItem.Idle) {
            root.dragImageGrab = TabItem.InFlightAndStale
            return
        }
        root.dragImageGrab = TabItem.InFlight
        let started = root.grabToImage(function(result) {
            root.dragImage = result.url
            let stale = root.dragImageGrab === TabItem.InFlightAndStale
            root.dragImageGrab = TabItem.Idle
            if (stale)
                root.refreshDragImage()
        })
        if (!started)
            root.dragImageGrab = TabItem.Idle
    }
    onWidthChanged: root.refreshDragImage()
    onVisibleChanged: root.refreshDragImage()
    onTabTitleChanged: root.refreshDragImage()
    onTabColorChanged: root.refreshDragImage()
    onTabActiveChanged: root.refreshDragImage()

    Rectangle {
        id: background
        objectName: "tabBackground"   // findChild() handle for the GUI test
        anchors.fill: parent
        // A pill is rounded; a joined tab's top corners are, its bottom meeting the content; the Flat strip's
        // tabs are square, as they always were.
        radius: root.kitTab ? chromeStyle.radii.tab : 0
        bottomLeftRadius: root.joined ? 0 : background.radius
        bottomRightRadius: root.joined ? 0 : background.radius
        color: root.effectiveBackground
    }

    // Keyboard focus. A pill rings outside itself, in the room its well leaves (TabStrip lets the list
    // overhang the well by the ring); a Flat tab fills its strip, so its ring is drawn inside it.
    FocusRing {
        shown: root.keyboardFocused
        reach: root.pill ? chromeStyle.size.focusRing : 0
        hostRadius: background.radius
        // Inside the tab the ring lies over its fill, which a Flat tab's own colour or the OS accent sets.
        ground: root.pill ? "transparent" : Qt.tint(systemPalette.window, root.effectiveBackground)
    }

    // A joined tab at rest ends in a rule, which sets it off the next one over the band; the active tab and
    // the strip's last need none.
    Rectangle {
        objectName: "tabJoinedRule"   // findChild() handle for the GUI test
        visible: root.joined && !root.tabActive && !root.lastInStrip
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: chromeStyle.borderWidth
        color: Qt.alpha(root.colors.tabText, chromeStyle.contrast.separatorAlpha)
    }

    // The tab colour as a cell of its own, on a row that draws colours as glyphs (the Terminal row's
    // "▌"): the colour roles keep the fill, so the colour needs a place of its own. Empty, and so zero
    // wide, for an uncoloured tab and on every row whose glyph is empty.
    Text {
        id: colorCell
        objectName: "tabColorCell"   // findChild() handle for the GUI test
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.kitTab && text !== ""
        width: visible ? implicitWidth : 0
        text: root.colored ? chromeStyle.tabColorGlyph : ""
        font: chromeStyle.font
        color: root.tabColor
    }

    // The pill's icon: the kit's tinted square (one per tab, whatever runs in it). A coloured tab fills
    // it with its colour, the active tab with the accent, the rest with the dimmed label colour. In an
    // icon-only pill the badge takes its place when there is one.
    Rectangle {
        id: icon
        objectName: "tabIcon"   // findChild() handle for the GUI test
        anchors.left: parent.left
        anchors.leftMargin: root.leadingInset
        anchors.verticalCenter: parent.verticalCenter
        visible: root.kitTab && !(root.iconOnly && (root.hasBadge || renameLoader.active))
        width: root.kitTab ? chromeStyle.size.tabIcon : 0
        height: width
        radius: chromeStyle.radii.tabIcon
        color: root.colored ? root.tabColor
             : root.tabActive ? chromeStyle.accentColor
             : root.colors.tabTextDimmed
    }

    // Progress indicator (OSC 9;4) for the tab's active pane: a thin bar along the tab's bottom edge,
    // under the label rather than beside it, so it costs the title no width and every tab keeps the
    // same layout whether or not an application is reporting progress. Inside a pill it is inset from
    // the pill's rounded ends and lifted off its bottom.
    Rectangle {
        id: progressTrack
        objectName: "progressTrack"  // findChild() handle for the GUI test
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.pill ? chromeStyle.space.s5 : 0
        anchors.rightMargin: root.pill ? chromeStyle.space.s5 : 0
        anchors.bottomMargin: root.pill ? chromeStyle.space.s1 : 0
        height: root.pill ? chromeStyle.size.progressBar : chromeStyle.borderWidth * 2
        radius: root.pill ? progressTrack.round : 0
        readonly property real round: height / 2
        // State 0 is Inactive: nothing in flight, so nothing to paint.
        visible: root.tabProgressState !== 0
        // The unfilled remainder, tinted from the tab's own foreground exactly as the zoom badge is, so
        // it tracks the active/inactive and colored/uncolored variants instead of hardcoding a color.
        color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.18)

        Rectangle {
            id: progressFill
            objectName: "progressFill"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            // Indeterminate (3) has no meaningful position, so the bar fills whole and pulses instead.
            width: root.tabProgressState === 3
                   ? parent.width
                   : parent.width * (root.tabProgressPercentage / 100.0)
            // The state->color mapping lives in the style's own token row, so this side neither
            // repeats it nor names the states. @see UiStyleProvider::progressColor.
            color: chromeStyle.progressColor(root.tabProgressState)
            radius: progressTrack.radius

            SequentialAnimation on opacity {
                running: root.tabProgressState === 3 && chromeStyle.motion.animates
                loops: Animation.Infinite
                // Restores full opacity when the state leaves Indeterminate mid-cycle; without it the
                // bar would keep whatever opacity the animation was stopped at. Addressed by id: an
                // animation attached with `on opacity` is not a visual child, so `parent` is null here.
                onRunningChanged: if (!running) progressFill.opacity = 1.0
                NumberAnimation { from: 1.0; to: 0.35; duration: chromeStyle.motion.pulse; easing.type: Easing.InOutQuad }
                NumberAnimation { from: 0.35; to: 1.0; duration: chromeStyle.motion.pulse; easing.type: Easing.InOutQuad }
            }
        }
    }

    HoverHandler {
        id: hover
        // Built when the hover STARTS, not bound: the working directory changes with every `cd`, and a
        // binding would have to be driven by a change signal the model does not emit. A tooltip is only
        // read while it is open, so composing it on open is both simpler and always current.
        onHoveredChanged: {
            if (!hovered)
                return
            root.hoverTooltip = root.buildHoverTooltip()
            root.refreshDragImage()
        }
    }

    // What the hover tooltip shows. Empty means "nothing worth saying", which suppresses it entirely.
    property string hoverTooltip: ""

    /// The full title (only when the tab is too narrow to show it) above the session's working directory.
    ///
    /// The directory is the point: with several tabs running full-screen applications, nothing on screen
    /// says where any of them was started, and the tab label is usually the application's name.
    function buildHoverTooltip() {
        var lines = []
        // What the badge says, first: it is the reason to hover over a badged tab.
        if (root.hasBadge && root.tabBadgeText !== "")
            lines.push(root.tabBadgeText)
        // A title the tab already shows in full adds nothing; repeating it is just noise.
        if (label.truncated)
            lines.push(root.tabTitle)
        var cwd = root.controller ? root.controller.tabWorkingDirectory(root.tabIndex) : ""
        if (cwd !== "")
            lines.push(cwd)
        return lines.join("\n")
    }

    // Its own ToolTip rather than the attached one, for the content item: the tooltip carries what a
    // program sent (its OSC 7501 message, the title it set), and a style's default Text.AutoText would
    // render that as markup -- colours, and an <img> the engine goes off to load -- whenever it looks like
    // some.
    ToolTip {
        id: hoverToolTip
        objectName: "tabHoverToolTip"   // findChild() handle for the GUI test
        text: root.hoverTooltip
        // Suppressed while renaming: the editor covers the label, and the tooltip would sit over the field
        // the user is typing into.
        visible: hover.hovered && root.hoverTooltip !== "" && !renameLoader.active
        delay: 600
        contentItem: Text {
            text: hoverToolTip.text
            textFormat: Text.PlainText
            font: chromeStyle.font
            color: hoverToolTip.palette.toolTipText
            wrapMode: Text.WordWrap
        }
    }

    // --- Tab drag: reorder within the strip, move into another window, split beside a pane, or tear out. ---
    // The tab drags as the node it is (NodeDrag.mime, the one payload a pane card carries too): the
    // controller decides what dropping it where does, and a tab of one pane drags as that pane. The real tab
    // stays put -- the OS drag cursor shows dragImage as the ghost.
    NodeDragSource {
        id: dragProxy
        objectName: "dragProxy"
        controller: root.controller
        // Read when the drag starts: what the tab holds then, not what it held when it was built. A controller
        // without drags starts none.
        begin: function() {
            return typeof root.controller.beginTabDrag === "function" ? root.controller.beginTabDrag(root.tabIndex)
                                                                      : ""
        }
        image: root.dragImage
    }

    DragHandler {
        id: dragHandler
        objectName: "tabDragHandler"   // findChild() handle for the GUI test
        target: null
        // Says what a pointer handler already defaults to, as the window-move handler in TitleBar does and
        // for the same reason: a tab is dragged with the left button, the right one belongs to the context
        // menu, and a default is a thing a future edit can change without anyone noticing.
        acceptedButtons: Qt.LeftButton
        // The default grabPermissions MINUS CanTakeOverFromItems, so this handler can never take a
        // gesture away from an item that already accepted the press.
        //
        // The window's resize handles are MouseAreas stacked above this strip (ResizeBorder, z:1000), and
        // the top-left corner one sits exactly on the first tab. A DragHandler takes a PASSIVE grab on
        // press, so it sees the moves regardless of who is on top; with CanTakeOverFromItems it then stole
        // the exclusive grab from the handle that had just asked the window manager to resize, and the one
        // press both resized the window and dragged this tab, whose QDrag ghost followed the cursor for
        // the whole gesture (issue #1997).
        //
        // The rule lives HERE rather than only on the resize handles because this is the half we can
        // guarantee: an item's keepMouseGrab (ResizeBorder's preventStealing) is a request Qt honours
        // differently across versions -- it holds on Qt 6.4 and 6.11 but not on 6.10 -- whereas
        // approveGrabTransition() refuses a handler without CanTakeOverFromItems outright. Nothing in the
        // strip holds the grab on press (the wheel MouseArea takes Qt.NoButton, the ListView is
        // interactive: false), so a genuine drag off a tab is unaffected.
        grabPermissions: PointerHandler.CanTakeOverFromHandlersOfDifferentType
                         | PointerHandler.ApprovesTakeOverByHandlersOfDifferentType
                         | PointerHandler.ApprovesTakeOverByItems
                         | PointerHandler.ApprovesCancellation
        // A small threshold so a click/tap still activates or renames; only a real drag begins a move.
        // Activating this handler starts the automatic QDrag (dragProxy.carry()); what the drop does is
        // decided by the DropArea that takes it and, where none does, by the controller, not here.
        dragThreshold: 8
        onActiveChanged: if (active) dragProxy.carry()
    }

    // The lock, before the title, while a pane of the tab is at a password prompt.
    SecureInputLock {
        id: lock
        objectName: "tabLock"   // findChild() handle for the GUI test
        indicator: root.secureState
        anchors.left: root.pill ? icon.right : colorCell.right
        anchors.leftMargin: root.pill ? chromeStyle.space.s3 : chromeStyle.labelPadding
        anchors.verticalCenter: parent.verticalCenter
        visible: root.lockShown
        color: root.foreground
        font: root.labelFont
        // What it takes from the title's row: itself and the gap after it, nothing while hidden.
        readonly property real room: visible ? width + root.partGap : 0
    }

    Text {
        id: label
        objectName: "tabLabel"   // findChild() handle for the GUI test
        anchors.left: lock.visible ? lock.right : root.kitTab ? icon.right : colorCell.right
        anchors.leftMargin: lock.visible ? root.partGap : root.kitTab ? root.kitGap : chromeStyle.labelPadding
        anchors.right: badgeSlot.left
        anchors.rightMargin: root.partGap
        anchors.verticalCenter: parent.verticalCenter
        // The chrome font: the platform UI font natively, the terminal's monospace font in the
        // terminal style, which is what puts the label on the same grid as the content below; the
        // kit's tab type on a pill.
        font: root.labelFont
        text: root.tabTitle
        color: root.foreground
        elide: Text.ElideRight
        visible: !renameLoader.active && !root.iconOnly
    }

    // The badge slot, between the label and the zoom badge, collapsing to nothing the way the zoom badge
    // does when there is none; in an icon-only pill it stands where the icon was. Its text is drawn as
    // text, never markup: the tooltip is what a program sent.
    TabBadgeSlot {
        id: badgeSlot
        objectName: "tabBadgeSlot"   // findChild() handle for the GUI test
        kind: root.tabBadge
        progress: root.tabBadgeProgress
        // Left of the zoom badge, or centred on the icon it stands in for. Positioned by x rather than
        // by anchors that detach, which an anchor layout does not undo reliably.
        x: root.iconOnly ? icon.x + (icon.width - width) / 2 : zoomBadge.x - gapAfter - width
        anchors.verticalCenter: parent.verticalCenter
        // The gap after the badge, which collapses with it.
        readonly property real gapAfter: root.hasBadge ? root.partGap : 0
    }

    // Zoom badge. A zoomed tab shows only one of its panes, which makes it look exactly like a
    // genuinely single-pane tab — this is the cue that the siblings are hidden rather than gone.
    // tmux marks the same state with a "Z", so the glyph should read as familiar rather than novel.
    Rectangle {
        id: zoomBadge
        objectName: "zoomBadge"   // findChild() handle for the GUI test
        anchors.right: closeButton.left
        // Collapse to nothing — width AND margin — when not zoomed. The label anchors to this item's
        // left edge, so anything it still occupies is stolen from every tab's title, zoomed or not:
        // a fixed margin here would elide the title 4px early on tabs that have no badge at all.
        anchors.rightMargin: zoomBadge.shown ? root.partGap : 0
        anchors.verticalCenter: parent.verticalCenter
        width: zoomBadge.shown ? chromeStyle.badgeWidth : 0
        height: chromeStyle.badgeHeight
        radius: chromeStyle.radius
        visible: zoomBadge.shown
        // An icon-only pill has no room for it.
        readonly property bool shown: root.tabZoomed && !root.iconOnly
        // Tinted from the tab's own foreground, so the badge tracks the active/inactive and
        // colored/uncolored variants instead of hardcoding a color per theme.
        color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.18)

        Text {
            anchors.centerIn: parent
            text: chromeStyle.zoomGlyph
            font: chromeStyle.badgeFont
            color: root.foreground
        }
    }

    /// Whether the tab has its own popup or editor open: the colour flyout or an inline rename. A
    /// full-screen band stays revealed while it does (see Main.qml).
    readonly property bool engaged: colorFlyout.visible || renameLoader.active

    // Inline rename field, shown on double-click.
    Loader {
        id: renameLoader
        active: false
        // Where the label is; an icon-only pill has no label to stand in for, so the field takes the
        // whole pill but its well padding (and the icon makes way, see there) rather than the negative
        // width between the icon and the badge.
        anchors.left: root.iconOnly ? parent.left : root.kitTab ? icon.right : colorCell.right
        anchors.leftMargin: root.iconOnly ? chromeStyle.space.s1 : label.anchors.leftMargin
        anchors.right: root.iconOnly ? parent.right : badgeSlot.left
        anchors.rightMargin: root.iconOnly ? chromeStyle.space.s1 : root.partGap
        anchors.verticalCenter: parent.verticalCenter
        function start() { active = true }
        sourceComponent: TextField {
            objectName: "tabRenameField"   // findChild() handle for the GUI test
            // Edit the RAW template (e.g. "a: {WindowTitle}"), not the expanded label the tab shows.
            // Empty for a never-renamed tab, so the editor opens blank.
            text: root.tabRawTitle
            selectByMouse: true
            Component.onCompleted: { forceActiveFocus(); selectAll() }
            // Guards the focus-out handler from re-finalizing an edit already finalized by Enter or
            // Escape (deactivating the Loader also drops focus, re-entering onActiveFocusChanged).
            property bool finalized: false
            // On accept the rename completes deterministically, so hand keyboard focus back to the
            // terminal (the field grabbed it on open). Escape cancels. The focus-out path does NOT
            // restore focus because focus has already moved to wherever the user clicked.
            onAccepted: {
                finalized = true
                root.controller.setTabTitle(root.tabIndex, text)
                renameLoader.active = false
                root.window.restoreTerminalFocus()
            }
            Keys.onEscapePressed: {
                finalized = true // cancel: discard the edit
                renameLoader.active = false
                root.window.restoreTerminalFocus()
            }
            // Clicking the terminal / another tab while editing COMMITS the typed name (matching the
            // common editor convention) rather than silently discarding it. Enter/Escape already
            // finalized (and set `finalized`), so this only runs for a genuine click-away. Do not
            // restore terminal focus here: focus has already moved to the user's click target, and
            // forcing it back would fight that.
            onActiveFocusChanged: {
                if (!activeFocus && !finalized) {
                    finalized = true
                    // Focus is also lost when the window tears down with the editor open; the
                    // controller is already null then, and there is no rename left to commit.
                    if (root.controller)
                        root.controller.setTabTitle(root.tabIndex, text)
                    renameLoader.active = false
                }
            }
        }
    }

    // The rule between this tab and the next. It lives inside the tab rather than between delegates
    // so the ListView keeps one item per tab -- the drop-slot math in TabStrip walks itemAtIndex()
    // and the drag payload is per delegate, both of which a wrapper Row would quietly break.
    //
    // A style that sets tabs flush reports an empty glyph, so this collapses to zero width and the
    // strip looks exactly as it always did.
    Text {
        id: separator
        objectName: "tabSeparator"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        // Nothing follows the last tab, so a rule there separates it from the "+" button rather than
        // from a neighbour -- it reads as a tab cut off at the edge of the strip. Dropping the TEXT
        // rather than hiding the item is what also returns its cell to that tab's label, because
        // naturalWidth above sums this item's width.
        //
        // No emptiness guard on the glyph itself: a style that sets tabs flush reports an empty one,
        // and an empty Text already has zero implicitWidth and paints nothing. Testing it here would
        // be the QML asking which style is active, which it must never do.
        text: root.lastInStrip ? "" : chromeStyle.tabSeparator
        font: chromeStyle.font
        color: root.foreground
    }

    ToolButton {
        id: closeButton
        objectName: "tabCloseButton"   // findChild() handle for the GUI test
        anchors.right: separator.left
        // The trailing counterpart of the label's leading inset. It is what keeps the glyph off
        // whatever ends the tab -- in a style that rules between tabs, the close button sat flush
        // against that rule, so "×│" read as one piece of box drawing instead of a button next to a
        // border. Zero natively, where tabs have no drawn edge. @see naturalWidth, which sums it.
        // A pill ends in the kit's s4 instead, and collapses the button while it is hidden, so a pill
        // at rest gives its title the room.
        anchors.rightMargin: root.kitTab ? chromeStyle.space.s4 : chromeStyle.trailingPadding
        anchors.verticalCenter: parent.verticalCenter
        visible: root.closeShown
        width: !root.kitTab ? chromeStyle.controlWidth : root.closeShown ? chromeStyle.size.tabClose : 0
        height: root.kitTab ? chromeStyle.size.tabClose : chromeStyle.controlHeight
        text: chromeStyle.closeGlyph
        Accessible.name: qsTr("Close tab %1").arg(root.tabTitle)
        // Don't take keyboard focus away from the terminal when clicked.
        focusPolicy: Qt.NoFocus
        onClicked: root.controller.closeTabAtIndex(root.tabIndex)
        // Flat, transparent chrome so the button blends into the tab fill instead of showing an opaque
        // style button panel; a subtle highlight wash gives hover feedback.
        background: Rectangle {
            radius: root.kitTab ? chromeStyle.radii.tabClose : 0
            color: !closeButton.hovered ? "transparent"
                 : root.joined && root.tabActive && !root.colored
                   ? chromeStyle.colorOn(chromeStyle.roles.tabCloseFill, root.contentColor)
                 : root.kitTab ? root.colors.tabCloseFill
                 : root.hoverWash
        }
        // The row's close mark (chromeStyle.layout.tabClose): its glyph, or the freedesktop close icon the
        // header bar's own buttons draw. Either shares the label's foreground, so the mark stays legible on
        // coloured fills and in light mode.
        readonly property bool symbolic: chromeStyle.layout.tabClose === chromeStyle.layout.symbolicIcon
        icon.name: closeButton.symbolic ? chromeStyle.layout.tabCloseIconName : ""
        icon.source: closeButton.symbolic ? chromeStyle.layout.tabCloseIconSource : ""
        icon.color: root.foreground
        icon.width: chromeStyle.size.headerIcon
        icon.height: chromeStyle.size.headerIcon
        contentItem: Loader {
            sourceComponent: closeButton.symbolic ? closeIconComponent : closeGlyphComponent
        }

        Component {
            id: closeGlyphComponent

            Text {
                objectName: "tabCloseMark"   // findChild() handle for the GUI test
                text: closeButton.text
                font: chromeStyle.closeFont
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: root.foreground
            }
        }

        Component {
            id: closeIconComponent

            SymbolicIcon {
                objectName: "tabCloseMark"   // findChild() handle for the GUI test
                icon: closeButton.icon
            }
        }
    }

    // A single left-tap activates the tab; a double-tap starts an inline rename. A TapHandler fires
    // onTapped for EVERY tap (tapCount is the running consecutive-tap count), so a double-tap fires
    // first with tapCount===1 (activate) then tapCount===2 (rename).
    //
    // Activate on the FIRST tap rather than deferring it by the double-click interval: activating an
    // already-active tab is a harmless no-op (SessionModel::activateTab early-outs), so the second tap of
    // a rename gesture re-activates the (already-active) tab and then opens the editor — you always rename
    // the tab you clicked. Deferring instead cost every single click the full double-click-interval latency
    // and, on platforms/styles where that interval is reported as 0, fired the deferred activation
    // synchronously so a genuine double-tap flashed the wrong tab before renaming.
    TapHandler {
        id: leftTap
        acceptedButtons: Qt.LeftButton
        // A press is where a drag begins, on a touch screen too, which has no hover to grab on.
        onPressedChanged: if (pressed) root.refreshDragImage()
        onTapped: (eventPoint, button) => {
            root.controller.activateTab(root.tabIndex)
            if (tapCount === 2)
                renameLoader.start()
        }
    }

    // The context menu is the strip's, one for every tab, built from the table the controller publishes:
    // the right-click asks the controller, which publishes the rows for THIS tab and asks the strip to pop
    // its menu under it. "Rename…" and "Custom…" come back as the requests the Connections below
    // already answer, once the menu has closed.
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: (eventPoint, button) => {
            if (root.controller)
                root.controller.openTabContextMenu(root.tabIndex,
                                                   root.mapToItem(null, eventPoint.position.x,
                                                                  eventPoint.position.y))
        }
    }

    // The swatch-grid color picker (WT-style quick coloring). It lives HERE rather than inside the
    // context menu because it has two entry points that must share one instance: "Custom…" in the
    // menu's "Tab Color", and the SetTabColor action below (which fires with no menu open at all).
    //
    // A Popup positions itself against its PARENT, which is now this tab rather than the menu that used
    // to own it. The flyout places itself against that parent (below the tab, or above it when the tab
    // strip sits at the bottom of the window) — see TabColorFlyout's own placement block.
    TabColorFlyout {
        id: colorFlyout
        objectName: "tabColorFlyout"
        controller: root.controller
        tabIndex: root.tabIndex
        // Which swatch the keyboard cursor starts on when the flyout opens.
        currentColor: root.tabColor
    }

    // Keyboard entry points (the SetTabTitle / SetTabColor actions): the WindowController emits
    // tabTitleEditRequested(index) / tabColorPickRequested(index) for the active tab; the matching
    // delegate opens its editor or its flyout. The active tab's delegate is always realized, so an
    // in-delegate Connections reliably fires.
    Connections {
        target: root.controller
        function onTabTitleEditRequested(index) {
            if (index === root.tabIndex)
                renameLoader.start()
        }
        function onTabColorPickRequested(index) {
            if (index === root.tabIndex)
                colorFlyout.open()
        }
    }

    // --- A pane or tab dragged onto this tab ---
    // Spring-loaded: resting on another tab for chromeStyle.tabDwellMs opens it under the drag, so the drag can
    // be dropped beside one of its panes; the dwell bar along the tab's foot fills meanwhile. Released on the
    // tab, the node lands beside the tab's active pane. The tab's edges are the strip's gaps (TabStrip.qml
    // takes those), so this takes the middle only: a tab narrower than four gap halves keeps half its width.
    DropArea {
        id: nodeDrop
        objectName: "tabDropArea"
        keys: [NodeDrag.mime]
        x: root.gapHalf
        width: Math.max(0, root.width - root.gapHalf - root.gapHalf)
        height: root.height

        /// What is dragged over the tab; the drag's JSON.
        property string nodePayload: ""
        /// Whether resting here opens the tab: the controller refuses the tab the drag came from and a
        /// window that cannot take the drag.
        property bool opensOnDwell: false

        onEntered: (drag) => {
            nodePayload = drag.getDataAsString(NodeDrag.mime)
            opensOnDwell = !root.tabActive && !!root.controller && root.controller.springOpens(root.tabIndex, nodePayload)
            if (opensOnDwell) {
                dwellTimer.restart()
                dwellFill.restart()
            }
            drag.accepted = true
        }
        onExited: {
            dwellTimer.stop()
            dwellFill.stop()
            dwellBar.progress = 0
            opensOnDwell = false
            nodePayload = ""
        }
        onDropped: (drop) => {
            dwellTimer.stop()
            dwellFill.stop()
            dwellBar.progress = 0
            const payload = drop.getDataAsString(NodeDrag.mime)
            opensOnDwell = false
            nodePayload = ""
            if (root.controller)
                root.controller.dropNodeOnTab(root.tabIndex, payload)
            // Every drop recognised is accepted, a refused one included, or the drag would end as a drop
            // outside every window and open one.
            drop.accept(Qt.MoveAction)
        }
    }

    /// How far each side of the tab belongs to the strip's gap rather than the tab (window::tabGapHalfWidthOf).
    readonly property real gapHalf: chromeStyle.tabGapHalfWidthOf(root.width)

    Timer {
        id: dwellTimer
        objectName: "tabDwellTimer"
        interval: chromeStyle.tabDwellMs
        onTriggered: {
            if (root.controller)
                root.controller.activateTab(root.tabIndex)
        }
    }

    // The tab says a node is over it: a ring in the accent.
    Rectangle {
        objectName: "tabDropMark"
        anchors.fill: parent
        visible: nodeDrop.containsDrag
        radius: background.radius
        color: "transparent"
        border.width: chromeStyle.dropEdgeWidth // the design's outline: 1.5 px in the accent
        border.color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.accentColor
    }

    // The dwell bar: fills along the tab's foot as the drag rests, and stays full once the tab has opened.
    Rectangle {
        id: dwellBar
        objectName: "tabDwellBar"
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        height: chromeStyle.space.s1
        /// How far the dwell has run, 0 to 1.
        property real progress: 0
        width: parent.width * progress
        visible: nodeDrop.containsDrag && nodeDrop.opensOnDwell
        color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.accentColor
        NumberAnimation {
            id: dwellFill
            target: dwellBar
            property: "progress"
            from: 0
            to: 1
            duration: chromeStyle.tabDwellMs
        }
    }
}
