// vim:syntax=qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Qt.labs.platform

ThemedWindow
{
    id: appWindow
    // NOT visible at declaration: the window is sized BEFORE its first map (win.showInitial() below)
    // from the real cell metrics, so it never shows at Qt's default size and then jumps. showInitial()
    // also applies the profile's window state (normal/maximized/fullscreen) as the first map.
    visible: false

    // show_title_bar (exposed as terminalSessions.titleBarVisible, sourced from the active pane's display)
    // selects the WINDOW DECORATION on every OS:
    //   - true  -> native, server-side decoration: keep the native frame (no FramelessWindowHint) and
    //              let the OS draw the min/max/close controls. Our tab strip still renders, but without
    //              its own window controls (they would duplicate the native ones).
    //   - false -> frameless + full client-side decoration: our tab strip draws everything, including
    //              the window controls. This is the default.
    // The frame is also kept in sync from C++ (TerminalDisplay::applyNativeTitleBar) for the
    // ToggleTitleBar action and the initial profile value; binding it here keeps QML and C++ agreeing
    // on the same axis.
    // The per-OS-window controller (WindowController), minted from the manager in Component.onCompleted
    // below. All window-scoped bindings (tab strip, title bar, active session, implicit size) go through
    // `win` so each in-process OS window is independent. Null until onCompleted runs; the bindings below
    // guard with `win &&` / `win ?` so they evaluate safely during the brief startup window.
    property var win: null

    // Whether the window goes frameless is no longer the negation of show_title_bar: it is per-OS,
    // and comes from the frame policy the controller applies (platform/NativeWindowFrame.hpp).
    // Qt.FramelessWindowHint is exactly what costs a window its system drop shadow and its rounded
    // corners, so Windows and macOS withhold it and keep a real frame instead -- Windows zeroes that
    // frame in WM_NCCALCSIZE, macOS lets the content fill the title bar. Only the
    // Linux/BSD desktops go genuinely frameless, and there the shadow comes from the compositor
    // (platform/WindowShadow.hpp) rather than from the frame. On macOS, Qt.ExpandedClientAreaHint is what
    // lets the content fill that title bar: Qt owns the full-size content view and its safe area, the
    // frame adapter does not (needsExpandedClientArea is true there alone, so other platforms never
    // evaluate the hint). The band is the client's own paint (ruling S6: under AppKit's title-bar material
    // the title bar would take the band's clicks), so Qt.NoTitleBarBackgroundHint clears the title bar's
    // background behind it (clearsTitleBarBackground, true only under the hint above).
    flags: Qt.Window | ((win && win.needsFramelessHint) ? Qt.FramelessWindowHint : 0)
                     | ((win && win.needsExpandedClientArea) ? Qt.ExpandedClientAreaHint : 0)
                     | ((win && win.clearsTitleBarBackground) ? Qt.NoTitleBarBackgroundHint : 0)
    // Under that hint the title bar becomes the window's safe area, and an ApplicationWindow (Qt >= 6.9) pads
    // its content by the safe area -- which would push the band below the very title bar it fills. Zeroed
    // while the hint is set alone; a Binding rather than `topPadding: 0`, because before Qt 6.9 (still
    // supported off macOS) the property does not exist, and an inactive Binding never names it.
    Binding
    {
        target: appWindow
        property: "topPadding"
        value: 0
        when: !!(win && win.needsExpandedClientArea)
    }

    // Application window's background must be transparent in order to support transparent/semi-transparent
    // background in the terminal widgets.
    color: "transparent"

    // The custom tab strip is shown regardless of the decoration mode — only the window controls inside
    // it are dropped when the native frame provides them (see TitleBar.useCustomWindowControls). Tabs
    // remain reachable whether or not the native title bar is used.
    //
    // Tab-strip placement + visibility come from the profile via the window controller (win). Both are
    // aliased to window-level properties here so the `win`-null startup guard (and its default) lives in
    // ONE place; the TitleBar/content bindings below read these, never `win` directly.
    //   - tabBarPosition: 0 = Top (above the terminal content), 1 = Bottom (below it). Mirrors the
    //     config::TabBarPosition enumerator order; defaults to Top before `win` resolves.
    //   - tabBarShouldShow: the resolved Always / Never / Multiple gate (against the live tab count);
    //     defaults to shown before `win` resolves.
    property int tabBarPosition: win ? win.tabBarPosition : 0
    property bool tabBarShouldShow: win ? win.tabBarShouldShow : true

    // Whether a full-screen band rests below the menu bar AppKit reveals over the window's top, as a native
    // full-screen toolbar does: a revealed band always (the pointer at the edge has revealed the menu bar
    // too), a pinned one (an interrupted remote connection) only while the pointer rests on the edge --
    // otherwise the menu bar is hidden, and a band below its place would float over live rows.
    readonly property bool bandBelowMenuBar: appWindow.band.band === "reveal" || appWindow.pinnedBandBelowMenuBar
    readonly property bool pinnedBandBelowMenuBar: appWindow.band.band === "pinned" && revealEdgeHover.hovered

    // The band's WindowFrame and WindowMode, as the ints titleBand() and bandFill() take.
    readonly property int bandFrame: appWindow.win && appWindow.win.titleBarVisible ? 1 : 0
    readonly property int bandMode: appWindow.visibility === Window.FullScreen ? 1 : 0

    // Where the title band and the tabs go -- { band, bandEdge, centre, tabBar }, each by name -- from the
    // row's tab shape (UiStyleProvider::titleBand): who draws the frame (show_title_bar), whether the window
    // is in full screen, the tab-bar gate, the configured edge and the host chip, whose interrupted
    // connection shows the band whatever the gate says. The ints are the enumerators' underlying values:
    // WindowFrame { ClientBand, NativeFrame }, WindowMode { Windowed, FullScreen }, TabStripGate { Hidden,
    // Shown }, TabBarPosition { Top, Bottom } and HostChipState. Main.qml is the only caller.
    readonly property var band: chromeStyle.titleBand(appWindow.bandFrame,
                                                      appWindow.bandMode,
                                                      appWindow.tabBarShouldShow ? 1 : 0,
                                                      appWindow.tabBarPosition,
                                                      (appWindow.win && appWindow.win.hostLabel) || "",
                                                      (appWindow.win && appWindow.win.hostChipState) || 0)

    // Whether a full-screen band should be revealed: while the pointer is on the reveal edge -- which,
    // once revealed, grows to the band's resting rectangle, so a band still sliding in is not lost to a
    // pointer moving down onto it -- or on the band, while the band has a popup or an editor of its own
    // open, which live in the overlay where no hover reaches, and while keyboard focus is on one of its
    // controls.
    function bandRevealWanted() {
        return appWindow.band.band === "reveal"
               && (revealEdgeHover.hovered
                   || (titleBar.revealed && (titleBar.hovered || titleBar.engaged || titleBar.holdsFocus
                                             || titleBarContextMenu.visible)));
    }

    // Reveals the band at once, and conceals it only after a short grace, which forgives a pointer that
    // overshoots the band's edge. Imperative rather than a binding: whether the band's own hover counts
    // depends on whether it is revealed, and a binding reading its own result would be a loop.
    function updateBandReveal() {
        if (appWindow.bandRevealWanted()) {
            bandHideDelay.stop();
            titleBar.revealed = true;
        } else if (appWindow.band.band !== "reveal") {
            bandHideDelay.stop();
            titleBar.revealed = false;
        } else if (titleBar.revealed && !bandHideDelay.running) {
            bandHideDelay.start();
        }
    }
    onBandChanged: appWindow.updateBandReveal()

    Timer {
        id: bandHideDelay
        interval: chromeStyle.motion.standard
        onTriggered: if (!appWindow.bandRevealWanted()) titleBar.revealed = false
    }

    // Diagnostic lifecycle/notification traces below are routed through a category that is off by
    // default (Warning floor), so they stay silent during normal use (closing a window and firing a
    // notification are expected events) but can be re-enabled for debugging via
    // QT_LOGGING_RULES="contour.window.debug=true".
    LoggingCategory {
        id: windowLog
        name: "contour.window"
        defaultLogLevel: LoggingCategory.Warning
    }

    // The window title follows the ACTIVE PANE's session: focusing another pane changes
    // terminalSessions.activeSession (which emits activeSessionChanged on a pane-focus change as well as on
    // a tab switch), and this binding also re-evaluates when that session's own title changes. Empty string
    // covers the brief startup window before the first session resolves. A window attached to a remote host
    // (`contour client --host`) names it first, so it is never mistaken for a local one.
    title: win ? win.windowTitle(win.activeSession ? win.activeSession.title : "") : ""

    // Create the first session/tab on startup. This used to be done by vtui's one-shot
    // `session: terminalSessions.createSession()`; with the pane tree as the sole renderer, Main.qml mints
    // the initial session so activeTabRootPane becomes a non-null single-leaf proxy the PaneNode Loader
    // renders. createNewTab() creates the session in THIS window (the controller tags it with its
    // WindowId).
    Component.onCompleted: {
        // Mint this OS window's controller BEFORE creating its first tab, so the tab's model events are
        // driven on this window's list-model.
        win = terminalSessions.createWindowController()
        // Bind the (still unmapped) window to the controller: adopts it as the geometry authority's
        // OS window, assigns the spawn target screen (the pre-show DPR predictor) and installs the
        // scale-settlement hooks.
        win.bindWindow(appWindow)
        // Declare the title-bar chrome to the controller. Window-geometry math (WM size hints,
        // programmatic window sizing, the initial size) uses this DECLARED value; measuring
        // window-minus-item instead is transiently wrong during relayout and structurally wrong in
        // splits.
        // The banner stack counts too: it takes rows from the terminal just as the strip does. A change
        // only refreshes the size hints -- it resizes no window, and no banner's height depends on the
        // window's -- so a banner coming or going cannot feed back into its own height.
        win.chromeHeight = Qt.binding(function() {
            return titleBar.effectiveHeight + tabsBar.effectiveHeight + bannerStack.height
        })
        // If this window was spawned to receive a torn-off tab (drag a tab onto the desktop),
        // adopt that tab as our sole tab instead of creating a fresh one — the dragged terminal
        // keeps running, no placeholder flash. Otherwise create the usual first tab. Either path
        // synchronously instantiates the pane tree -> TerminalDisplay -> renderer (headless cell
        // metrics), so showInitial() below can size the window from REAL metrics before the first map.
        if (!terminalSessions.consumePendingTransplant(win)
            && !terminalSessions.consumeAttachWindow(win)
            && !terminalSessions.consumeDefaultLayout(win))
            win.createNewTab()
        win.showInitial()
    }

    // {{{ Custom title bar + terminal content
    TitleBar {
        id: titleBar
        objectName: "titleBar" // so offscreen layout tests can findChild() this item
        // Pinned to the top OR the bottom edge via a single `y` binding — never a pair of conditional
        // top/bottom anchors, which a Bottom->Top flip would leave BOTH set for an instant, stretching the
        // bar to the window's height and overriding the `height` binding below for good. See the scrollbar
        // in SessionChrome.qml for the full mechanism; it shipped that bug as a full-pane-wide bar.
        // left/right stay anchored: that pair is unconditional, so sizing the width is intended.
        // A full-screen band slides from above its resting place by its slideOffset.
        anchors.left: parent.left
        anchors.right: parent.right
        y: titleBar.restingTop + titleBar.slideOffset
        // At the bottom edge, or at the top -- where a full-screen band rests below the menu bar AppKit
        // reveals over the window (bandBelowMenuBar), as the controller measured it; 0 off macOS.
        restingTop: appWindow.band.bandEdge === "bottom" ? parent.height - titleBar.height
                  : appWindow.bandBelowMenuBar ? ((appWindow.win && appWindow.win.titleBandRevealTop) || 0) : 0
        // A revealed band slides in and out; a pinned one slides between the top and its place below the
        // menu bar. Under Reduce Motion neither slides: the revealed band fades, the pinned one moves at once.
        Behavior on y {
            enabled: (titleBar.presence === "reveal" || titleBar.presence === "pinned")
                     && chromeStyle.motion.animates
            NumberAnimation {
                id: bandSlide // so offscreen tests can tell a slide from a jump without sampling y
                duration: chromeStyle.motion.standard
            }
        }
        // Shown per the band table: on macOS always (it carries the traffic lights), elsewhere per the
        // profile's tab_bar_visibility resolved against the live tab count (Always / Never / Multiple). When
        // hidden, height/effectiveHeight collapse to 0 so the content area (and the declared chrome height
        // feeding the geometry authority) reclaim it. A full-screen band overlays the top rows instead, so
        // it never takes room from the content (effectiveHeight 0) and is visible only while it reveals,
        // or still fading out.
        presence: appWindow.band.band
        centre: appWindow.band.centre
        // A concealed "reveal" band leaves the screen and the accessibility tree once it has slid away (or,
        // under Reduce Motion, faded out). A "pinned" one overlays the top rows like a revealed one, held there
        // while the remote connection is interrupted.
        visible: presence === "shown" || presence === "pinned"
                 || (presence === "reveal"
                     && (revealed || (chromeStyle.motion.animates ? y > -height : opacity > 0)))
        height: presence === "hidden" ? 0 : implicitHeight
        // The band's height as the window measured it, grown to the native title bar it covers (macOS);
        // `|| 0` (no measurement) keeps the token. The tabs bar below carries no traffic lights: it keeps
        // the token.
        measuredHeight: (appWindow.win && appWindow.win.titleBandHeight) || 0
        // The band's surface: the frame and mode it is drawn in, as titleBand() was asked, and the centre it
        // answered. Reads paletteRevision first, since the fill reads the palette.
        fill: chromeStyle.paletteRevision < 0 ? "transparent"
              : chromeStyle.bandFill(appWindow.bandFrame, appWindow.bandMode, appWindow.band.centre || "")
        readonly property real effectiveHeight: presence === "shown" ? implicitHeight : 0
        onHoveredChanged: appWindow.updateBandReveal()
        onEngagedChanged: appWindow.updateBandReveal()
        onHoldsFocusChanged: appWindow.updateBandReveal()
        // Declared after the content area below, so a positive z keeps the bar above it. The terminal now
        // renders through a scene-graph node (TerminalRenderNode) and composites in z-order like any item,
        // so this is ordinary stacking — no longer a workaround for the terminal painting over siblings.
        z: 2
        controller: appWindow.win
        window: appWindow
        // Draw our own min/max/close controls only in frameless (CSD) mode. When the native frame is
        // shown (titleBarVisible), the OS draws those controls, so ours would duplicate them.
        // Not the negation of titleBarVisible: on macOS the window stays a decorated NSWindow whose
        // OWN traffic lights are drawn even with our tab strip, so ours must yield there too or both
        // sets appear. The policy decides; this only asks.
        useCustomWindowControls: !!(appWindow.win && appWindow.win.needsClientFrameAffordances)
        // `|| 0` for the same reason the gates above use `!!`: a controller that predates this
        // property answers undefined, and assigning that to a real is a QML error. In full screen AppKit
        // draws the traffic lights in the strip it reveals below the menu bar, over a band resting there
        // (bandBelowMenuBar) -- the controller publishes their inset there too -- and nowhere over a
        // pinned band resting at the top while the menu bar is hidden.
        nativeControlsInset: appWindow.bandMode === 1 && !appWindow.bandBelowMenuBar ? 0
                             : ((appWindow.win && appWindow.win.nativeControlsInset) || 0)
    }

    // The bar that holds only the tabs, at the bottom, while the band stays at the top with the title (the
    // Pill shape with the tabs configured at the bottom). No controls, chips or toolbar: those are the band's.
    TitleBar {
        id: tabsBar
        objectName: "tabsBar" // so offscreen layout tests can findChild() this item
        role: "tabs"
        anchors.left: parent.left
        anchors.right: parent.right
        y: parent.height - tabsBar.height
        presence: appWindow.band.tabBar
        // Its tabs exist only while it is shown, so a hidden one holds no second strip or tab menu.
        centre: presence === "shown" ? "tabs" : "empty"
        visible: presence === "shown"
        height: visible ? implicitHeight : 0
        readonly property real effectiveHeight: height
        z: 2
        controller: appWindow.win
        window: appWindow
        useCustomWindowControls: false
    }

    // The strip along the top edge that reveals a full-screen band while the pointer rests on it, and moves a
    // pinned one below the menu bar the pointer reveals there.
    Item {
        id: bandRevealEdge
        objectName: "bandRevealEdge" // so offscreen tests can findChild() it
        anchors.left: parent.left
        anchors.right: parent.right
        y: 0
        // Down to the band's resting rectangle while it is revealed, so the pointer keeps it however far it
        // has slid, and on its way down from the menu bar above it; passive, so every click still reaches
        // the band beneath.
        height: titleBar.revealed || appWindow.pinnedBandBelowMenuBar
                ? titleBar.restingTop + titleBar.implicitHeight : chromeStyle.space.revealEdge
        // A pinned band is shown already; its edge only moves it below the menu bar (bandBelowMenuBar).
        visible: appWindow.band.band === "reveal" || appWindow.band.band === "pinned"
        z: 3

        HoverHandler {
            id: revealEdgeHover
            onHoveredChanged: appWindow.updateBandReveal()
        }
    }

    // The banners, in a stack above the content: the content starts below them, so a banner pushes the
    // terminal down (and back up when it goes) instead of covering its first line. With the tab strip
    // at the top the stack sits below it; at the bottom it is the window's topmost row. A banner with no
    // text collapses to zero height, so an empty stack takes no room.
    Column {
        id: bannerStack
        objectName: "bannerStack" // so offscreen tests can findChild() it
        anchors.left: parent.left
        anchors.right: parent.right
        // Below the band's effective height: a full-screen band overlays the content and pushes nothing.
        y: appWindow.band.bandEdge === "top" ? titleBar.effectiveHeight : 0
        // The banners' own extent, taken at once: the Column's implicit height follows only when the
        // window next polishes, and the content below is anchored to this edge. Summed rather than
        // childrenRect, which keeps a collapsed banner's stale y -- dismiss the lower banner first and
        // the upper one second, and the stack would keep the upper one's height.
        height: hostingNoticeBar.height + newDefaultsBanner.height
        z: 1

        // Shown while this window runs its shells itself because the daemon the launch wanted could not be
        // had (WindowController.hostingNotice): they end when the window closes, which a daemon-hosted
        // window's do not.
        Banner {
            id: hostingNoticeBar
            objectName: "hostingNoticeBar" // so offscreen tests can findChild() it
            width: parent.width
            // `|| ""` for the reason the gates above use `!!`: a lightweight mock `win` lacks the property.
            text: (appWindow.win && appWindow.win.hostingNotice) || ""
            onDismissed: appWindow.win.dismissHostingNotice()
        }

        // Offers the newest defaults to a configuration that predates them (window::NewDefaultsNotice).
        // Guarded with typeof, as DaemonDriftBadge is: the test harnesses set no such context property.
        Banner {
            id: newDefaultsBanner
            objectName: "newDefaultsBanner" // so offscreen tests can findChild() it
            readonly property var notice: typeof newDefaults !== "undefined" ? newDefaults : null
            width: parent.width
            text: notice !== null && notice.offered ? qsTr("Contour has new defaults") : ""
            actionText: qsTr("Use New")
            dismissText: qsTr("Keep Mine")
            onActionTriggered: newDefaultsBanner.notice.useNew()
            onDismissed: newDefaultsBanner.notice.keepMine()
        }
    }

    // Content area: the active tab's pane tree is the SOLE renderer for every case — a single unsplit
    // terminal is just a single-leaf tree (one TerminalPane), and a split is a multi-leaf tree. There is no
    // separate single-pane display: this removes the vtui<->pane-tree session hand-off that caused a series
    // of split lifecycle bugs. The Loader is active whenever the active tab has a root pane (always, once the
    // startup session exists).
    Item {
        id: content
        objectName: "content" // so offscreen layout tests can findChild() this item
        // Fills the window edge-to-edge on the axis the bars do NOT occupy: below the band and the banners
        // at the top, and down to the band, or to the tabs bar, at the bottom. A hidden bar's height is 0,
        // so content reclaims the full window either way.
        anchors.top: bannerStack.bottom
        anchors.bottom: appWindow.band.bandEdge === "bottom" ? titleBar.top : tabsBar.top
        anchors.left: parent.left
        anchors.right: parent.right

        // Every tooltip is an attached one, and the engine shares one instance among them, built by the
        // style in use. Fusion's (`ui_style: native`) reads its text as AutoText, which renders a link's URI,
        // a tab title or a command line as rich text whenever its first line looks like markup -- an
        // `<img src=...>` fetched on hover. Set here, on an Item, as the attached property requires.
        Component.onCompleted: ToolTip.toolTip.contentItem.textFormat = Text.PlainText

        // The terminal content: the active tab's pane tree. Settings is a window of its own
        // (SettingsWindow.qml), so nothing else ever takes this area's place.
        Loader {
            id: paneContentLoader
            anchors.fill: parent
            active: appWindow.win !== null && appWindow.win.activeTabRootPane !== null
            sourceComponent: PaneNode {
                node: appWindow.win ? appWindow.win.activeTabRootPane : null
                visiblePanes: appWindow.win ? appWindow.win.visiblePaneCount : 1
                headerReveal: appWindow.win ? appWindow.win.headerReveal : 0
            }
        }

    }

    // Shown only while the daemon runs another release than this client (see DaemonDriftModel): in the
    // band's trailing area while the band is there -- shown, or revealing with it in full screen, never over
    // the cells -- and at the content's corner while it is not, so the drift is never unsaid. Positioned by
    // x/y, since its parent changes.
    DaemonDriftBadge {
        id: driftChip
        readonly property bool inBand: titleBar.presence !== "hidden"
        parent: inBand ? titleBar.driftSlot : content
        x: inBand ? 0 : parent.width - width - chromeStyle.space.s4
        y: inBand ? (parent.height - height) / 2 : parent.height - height - chromeStyle.space.s4
        z: 10
    }

    // The command palette (Ctrl+Shift+P). Opened by the OpenCommandPalette action, which routes
    // through the session manager to THIS window's controller — so the popup appears over the window
    // the chord was pressed in, not over every open window. It is a Popup, so it lives in the window's
    // overlay layer and sits above the terminal content and the resize border without needing a z.
    CommandPalette {
        id: commandPalette
        objectName: "commandPalette" // so offscreen tests can findChild() it
        controller: appWindow.win
        window: appWindow
    }

    // The "save layout as" name prompt. Opened by a nameless SaveLayout action (the command-palette row
    // or a key bound to bare SaveLayout), routed per-window exactly like the command palette above, so
    // the prompt appears over the window the action fired in.
    SaveLayoutDialog {
        id: saveLayoutDialog
        controller: appWindow.win
        window: appWindow
    }

    // The terminal pane's right-click menu. Per-window like the command palette: the OpenContextMenu
    // action routes through the session manager to THIS window's controller, which first makes the
    // right-clicked pane active, then republishes the model for the state under the cursor and asks us to
    // pop it. popup() with no arguments opens at the mouse cursor — which is the click position, since the
    // whole chain runs synchronously inside the mouse-press handler.
    ActionContextMenu {
        id: terminalContextMenu
        entries: appWindow.win ? appWindow.win.contextMenuModel : []
        onPicked: (actionId) => { if (appWindow.win) appWindow.win.triggerContextMenuAction(actionId); }
    }

    // The title bar's own menu: a second instance of the same component, differing only in which model
    // it renders and which controller method a pick runs.
    ActionContextMenu {
        id: titleBarContextMenu
        entries: appWindow.win ? appWindow.win.titleBarContextMenuModel : []
        // A full-screen band stays while its own menu is open (see bandRevealWanted).
        onVisibleChanged: appWindow.updateBandReveal()
        onPicked: (actionId) => {
            if (appWindow.win) appWindow.win.triggerTitleBarContextMenuAction(actionId);
        }
    }

    // The About dialog: an in-scene KitDialog, not AppKit's panel, so it carries the chrome row's look.
    // Opened by the ShowAbout action; one button, since there is nothing to decline.
    KitDialog {
        id: aboutDialog
        objectName: "aboutDialog"
        heading: qsTr("Contour")
        message: qsTr("Version %1").arg(appWindow.win ? appWindow.win.applicationVersion : "")
        acceptText: qsTr("OK")
        rejectText: ""
    }

    Connections {
        target: appWindow.win
        function onCommandPaletteRequested() { commandPalette.open(); }
        function onAboutRequested() { aboutDialog.open(); }
        function onSaveLayoutRequested() { saveLayoutDialog.open(); }
        function onContextMenuRequested() { terminalContextMenu.popup(); }
        function onTitleBarContextMenuRequested() { titleBarContextMenu.popup(); }
        // Attention on behalf of a session that may have no pane on screen -- a command finishing in a
        // background tab. Such a session has no SessionChrome, so the window carries the request.
        function onNotificationRequested(title, body) { appWindow.showTrayMessage(title, body); }
        function onBellRequested(source, volume) { windowBell.play(source, volume); }
        function onAlertRequested() { appWindow.alert(0); }
    }

    // The bell the window plays for a session without a SessionChrome of its own, deferred exactly as
    // SessionChrome's is until multimedia is ready.
    DeferredBell {
        id: windowBell
        objectName: "windowBell" // so offscreen tests can findChild() it
    }

    // Frameless windows need their own edge/corner resize handles. With the native frame
    // (titleBarVisible), the WM already draws an edge resize border, so ours would duplicate it
    // and — being invisible z:1000 hit zones around every edge — would steal edge clicks (scrollbar
    // drags, text selection near a border) and start a system resize instead. Mirror the same
    // frameless gate WindowControls uses (TitleBar.useCustomWindowControls). visible:false also stops
    // hit-testing the MouseAreas, so the border is fully inert in native-frame mode.
    ResizeBorder {
        window: appWindow
        // Same policy as the window controls: where the OS keeps a real frame it resizes the window
        // itself, and our hit zones would only compete with it.
        visible: !!(appWindow.win && appWindow.win.needsClientFrameAffordances)
    }
    // }}}

    // Return keyboard focus to the terminal after any interaction with the tab strip / window
    // controls, so the user can keep typing without having to click back into the terminal. The pane tree
    // is the sole content; forwarding focus to the loaded PaneNode lands it on the active leaf (each
    // TerminalPane grabs focus when it becomes the active pane, see TerminalPane.onPaneActiveChanged).
    function restoreTerminalFocus() {
        if (paneContentLoader.item)
            paneContentLoader.item.forceActiveFocus();
    }

    // Restore focus to the terminal whenever the active tab changes (tab click / switch).
    Connections {
        target: appWindow.win
        function onActiveTabIndexChanged() { appWindow.restoreTerminalFocus(); }
    }

    onClosing: {
        console.debug(windowLog, "Terminal closed. Removing session.");
        if (appWindow.win)
            appWindow.win.closeWindow();
    }

    // A notice a pane on screen relays from its session: a paste too large, a screenshot taken. Through the
    // window, which so knows what the tray's next click is on.
    function showNotification(title, content) {
        let message = "Application Message: %1".arg(title);
        if (appWindow.win)
            appWindow.win.requestNotification(message, content);
        else
            showTrayMessage(message, content);
    }

    // One tray message. What a session's notifier raises -- a command finishing, or an application's
    // OSC 777, OSC 9 or OSC 99 from any tab -- comes here directly, already titled.
    function showTrayMessage(title, content) {
        console.debug(windowLog, "main: notification [%1] %2".arg(title).arg(content));
        if (trayIcon.supportsMessages)
        {
            trayIcon.show();
            trayIcon.showMessage(title, "%1".arg(content), appWindow.trayMessageTimeout);
            trayIconTimer.restart();
        }
        else
            console.warn("main: Notification system not supported!");
    }

    // How long a tray message stays up, in milliseconds -- and so how long its icon stays in the tray.
    readonly property int trayMessageTimeout: 60 * 1000

    // The tray icon exists only to carry a message, so it leaves once the message is over: timed out,
    // clicked, or made moot by the user coming back to the window. Without this, every window that
    // ever announced a background build would keep an icon in the tray for good.
    Timer {
        id: trayIconTimer
        objectName: "trayIconTimer" // so offscreen tests can findChild() it
        interval: appWindow.trayMessageTimeout
        onTriggered: trayIcon.hide()
    }

    onActiveChanged: {
        if (active) {
            trayIconTimer.stop();
            trayIcon.hide();
        }
    }

    // NB: This requires Qt 5.12+
    // See https://doc.qt.io/qt-5/qml-qt-labs-platform-systemtrayicon.html#availability for details.
    SystemTrayIcon {
        id: trayIcon
        objectName: "trayIcon" // so offscreen tests can findChild() it
        visible: false
        icon.source: "qrc:/contour/logo-256.png"
        icon.name: "Contour Terminal"

        menu: Menu {
            MenuItem {
                text: qsTr("Quit")
                onTriggered: Qt.quit()
            }
        }

        // A click brings the window forward and does what the message asked of it, such as revealing the
        // session it is about. The click carries no identity of the message: the window takes it for one
        // on the last message shown.
        onMessageClicked: { hide(); if (appWindow.win) appWindow.win.trayMessageClicked(); }
    }
}
