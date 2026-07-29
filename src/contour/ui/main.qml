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

    flags: (win && win.titleBarVisible) ? Qt.Window : (Qt.Window | Qt.FramelessWindowHint)

    // Application window's background must be transparent in order to support transparent/semi-transparent
    // background in the terminal widgets.
    // color: "transparent"
    color: Qt.rgba(0, 0, 0, 0.0)

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
    // covers the brief startup window before the first session resolves.
    title: (win && win.activeSession) ? win.activeSession.title : ""

    // Create the first session/tab on startup. This used to be done by vtui's one-shot
    // `session: terminalSessions.createSession()`; with the pane tree as the sole renderer, main.qml mints
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
        win.chromeHeight = Qt.binding(function() { return titleBar.effectiveHeight })
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
        useCustomWindowControls: !(appWindow.win && appWindow.win.titleBarVisible)
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
        anchors.top: appWindow.tabBarPosition === 0 ? titleBar.bottom : parent.top
        anchors.bottom: appWindow.tabBarPosition === 1 ? titleBar.top : parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right

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
            anchors.fill: parent
            active: appWindow.win !== null && appWindow.win.settingsActive === true
            visible: active
            sourceComponent: SettingsPage {
                controller: appWindow.win ? appWindow.win.settingsController : null
                windowController: appWindow.win
            }
        }
    }

    // The command palette (Ctrl+Shift+P). Opened by the OpenCommandPalette action, which routes
    // through the session manager to THIS window's controller — so the popup appears over the window
    // the chord was pressed in, not over every open window. It is a Popup, so it lives in the window's
    // overlay layer and sits above the terminal content and the resize border without needing a z.
    CommandPalette {
        id: commandPalette
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

    Connections {
        target: appWindow.win
        function onCommandPaletteRequested() { commandPalette.open(); }
        function onSaveLayoutRequested() { saveLayoutDialog.open(); }
        function onContextMenuRequested() { terminalContextMenu.popup(); }
        function onTitleBarContextMenuRequested() { titleBarContextMenu.popup(); }
    }

    // Frameless windows need their own edge/corner resize handles. With the native frame
    // (titleBarVisible), the WM already draws an edge resize border, so ours would duplicate it
    // and — being invisible z:1000 hit zones around every edge — would steal edge clicks (scrollbar
    // drags, text selection near a border) and start a system resize instead. Mirror the same
    // frameless gate WindowControls uses (TitleBar.useCustomWindowControls). visible:false also stops
    // hit-testing the MouseAreas, so the border is fully inert in native-frame mode.
    ResizeBorder {
        window: appWindow
        visible: !(appWindow.win && appWindow.win.titleBarVisible)
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

    function showNotification(title, content) {
        // "OSC 777 ; notify ; <TITLE> ; <CONTENT> ST"
        // Example: printf "\033]777;notify;Hello Title;Hello Content\033\\"
        console.debug(windowLog, "main: notification [%1] %2".arg(title).arg(content));
        if (trayIcon.supportsMessages)
        {
            trayIcon.show();
            trayIcon.showMessage("Application Message: %1".arg(title),
                                 "%1".arg(content),
                                 60 * 1000);
        }
        else
            console.warn("main: Notification system not supported!");
    }

    // NB: This requires Qt 5.12+
    // See https://doc.qt.io/qt-5/qml-qt-labs-platform-systemtrayicon.html#availability for details.
    SystemTrayIcon {
        id: trayIcon
        visible: false
        icon.source: "qrc:/contour/logo-256.png"
        icon.name: "Contour Terminal"

        menu: Menu {
            MenuItem {
                text: qsTr("Quit")
                onTriggered: Qt.quit()
            }
        }

        onMessageClicked: hide()
    }
}
