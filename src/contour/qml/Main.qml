// vim:syntax=qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Qt.labs.platform

ApplicationWindow
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
    // frame in WM_NCCALCSIZE, macOS lets the content fill a transparent title bar. Only the
    // Linux/BSD desktops go genuinely frameless, and there the shadow comes from the compositor
    // (platform/WindowShadow.hpp) rather than from the frame.
    flags: Qt.Window | ((win && win.needsFramelessHint) ? Qt.FramelessWindowHint : 0)

    // Application window's background must be transparent in order to support transparent/semi-transparent
    // background in the terminal widgets.
    color: "transparent"

    // Force the window's Qt Quick Controls palette to follow the application palette (which
    // ContourGuiApp::applyGuiTheme drives for the theme: dark|light|system setting).
    //
    // Why this is needed: the pinned Fusion Quick Controls style derives an editable control's colors
    // (TextField/ComboBox/SpinBox `base`, `text`, …) from QStyleHints::colorScheme(), NOT from the
    // application QPalette. On desktops whose platform theme owns the color scheme (KDE Plasma, GNOME),
    // QStyleHints::setColorScheme() is inert, so those controls render in the OS scheme (e.g. dark input
    // fields) even when the forced GUI theme — and every SystemPalette-bound chrome element — is light.
    // Binding the window palette to a SystemPalette (which DOES follow the application palette) makes the
    // controls inherit an explicit palette that wins over the style default, so the whole window,
    // including the on-demand settings pane, is themed consistently from first render. In `theme: system`
    // this simply mirrors the OS palette, i.e. a no-op.
    SystemPalette
    {
        id: appPalette
        colorGroup: SystemPalette.Active
    }
    palette.window: appPalette.window
    palette.windowText: appPalette.windowText
    palette.base: appPalette.base
    palette.alternateBase: appPalette.alternateBase
    palette.text: appPalette.text
    palette.button: appPalette.button
    palette.buttonText: appPalette.buttonText
    palette.brightText: appPalette.brightText
    palette.highlight: appPalette.highlight
    palette.highlightedText: appPalette.highlightedText
    palette.toolTipBase: appPalette.toolTipBase
    palette.toolTipText: appPalette.toolTipText
    palette.placeholderText: appPalette.placeholderText
    palette.light: appPalette.light
    palette.midlight: appPalette.midlight
    palette.mid: appPalette.mid
    palette.dark: appPalette.dark
    palette.shadow: appPalette.shadow

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
        win.chromeHeight = Qt.binding(function() { return titleBar.effectiveHeight + bannerStack.height })
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
        anchors.left: parent.left
        anchors.right: parent.right
        y: appWindow.tabBarPosition === 0 ? 0 : parent.height - titleBar.height
        // Shown per the profile's tab_bar_visibility, resolved against the live tab count by the
        // controller (Always / Never / Multiple). When hidden, height/effectiveHeight collapse to 0 so
        // the content area (and the declared chrome height feeding the geometry authority) reclaim it.
        visible: appWindow.tabBarShouldShow
        height: visible ? implicitHeight : 0
        property real effectiveHeight: visible ? implicitHeight : 0
        // Declared after the content area below, so a positive z keeps the bar above it. The terminal now
        // renders through a scene-graph node (TerminalRenderNode) and composites in z-order like any item,
        // so this is ordinary stacking — no longer a workaround for the terminal painting over siblings.
        z: 1
        controller: appWindow.win
        window: appWindow
        // Draw our own min/max/close controls only in frameless (CSD) mode. When the native frame is
        // shown (titleBarVisible), the OS draws those controls, so ours would duplicate them.
        // Not the negation of titleBarVisible: on macOS the window stays a decorated NSWindow whose
        // OWN traffic lights are drawn even with our tab strip, so ours must yield there too or both
        // sets appear. The policy decides; this only asks.
        useCustomWindowControls: !!(appWindow.win && appWindow.win.needsClientFrameAffordances)
        // `|| 0` for the same reason the gates above use `!!`: a controller that predates this
        // property answers undefined, and assigning that to a real is a QML error.
        nativeControlsInset: (appWindow.win && appWindow.win.nativeControlsInset) || 0
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
        y: appWindow.tabBarPosition === 0 ? titleBar.height : 0
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
        // Fills the window edge-to-edge on the axis the tab strip does NOT occupy: with the strip at
        // the top, content runs from titleBar.bottom to the window bottom; with the strip at the bottom,
        // content runs from the window top to titleBar.top. When the strip is hidden, titleBar's height
        // is 0, so content reclaims the full window either way.
        anchors.top: bannerStack.bottom
        anchors.bottom: appWindow.tabBarPosition === 1 ? titleBar.top : parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right

        // Every tooltip is an attached one, and the engine shares one instance among them, built by the
        // style in use. Fusion's (`ui_style: native`) reads its text as AutoText, which renders a link's URI,
        // a tab title or a command line as rich text whenever its first line looks like markup -- an
        // `<img src=...>` fetched on hover. Set here, on an Item, as the attached property requires.
        Component.onCompleted: ToolTip.toolTip.contentItem.textFormat = Text.PlainText

        // The terminal content. Kept ACTIVE (so the pane tree and its PTYs keep running) even while the
        // settings page is shown, just hidden — flipping back is instant and nothing is torn down.
        Loader {
            id: paneContentLoader
            anchors.fill: parent
            active: appWindow.win !== null && appWindow.win.activeTabRootPane !== null
            // `=== true` coerces a missing/undefined property (lightweight mock `win` in tests) to a bool.
            visible: !(appWindow.win !== null && appWindow.win.settingsActive === true)
            sourceComponent: PaneNode {
                node: appWindow.win ? appWindow.win.activeTabRootPane : null
            }
        }

        // The in-app settings page, shown in place of the terminal when the WindowController's
        // settingsActive flag is set (the tab-strip gear, or the OpenConfiguration action). A peer of the
        // pane-tree Loader, honoring the "single renderer" architecture — settings is not a pane.
        Loader {
            id: settingsPageLoader
            objectName: "settingsPageLoader" // so offscreen tests can findChild() it
            anchors.fill: parent
            active: appWindow.win !== null && appWindow.win.settingsActive === true
            visible: active
            sourceComponent: SettingsPage {
                controller: appWindow.win ? appWindow.win.settingsController : null
                windowController: appWindow.win
            }
        }

        // Shown only while the daemon runs another release than this client (see DaemonDriftModel).
        DaemonDriftBadge {
            z: 10
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 8
        }
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
