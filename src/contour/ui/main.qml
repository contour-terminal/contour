// vim:syntax=qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Qt.labs.platform

ApplicationWindow
{
    id: appWindow
    visible: true

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
    flags: terminalSessions.titleBarVisible ? Qt.Window : (Qt.Window | Qt.FramelessWindowHint)

    // Application window's background must be transparent in order to support transparent/semi-transparent
    // background in the terminal widgets.
    // color: "transparent"
    color: Qt.rgba(0, 0, 0, 0.0)

    // The custom tab strip is shown regardless of the decoration mode — only the window controls inside
    // it are dropped when the native frame provides them (see TitleBar.useCustomWindowControls). Tabs
    // remain reachable whether or not the native title bar is used.
    property bool showCustomTitleBar: true

    // The window title follows the ACTIVE PANE's session: focusing another pane changes
    // terminalSessions.activeSession (which emits activeSessionChanged on a pane-focus change as well as on
    // a tab switch), and this binding also re-evaluates when that session's own title changes. Empty string
    // covers the brief startup window before the first session resolves.
    title: terminalSessions.activeSession ? terminalSessions.activeSession.title : ""

    // Create the first session/tab on startup. This used to be done by vtui's one-shot
    // `session: terminalSessions.createSession()`; with the pane tree as the sole renderer, main.qml mints
    // the initial session so activeTabRootPane becomes a non-null single-leaf proxy the PaneNode Loader
    // renders. createNewTab() allows creation (the Windows _allowCreation guard) then creates the session.
    Component.onCompleted: {
        terminalSessions.createNewTab()
    }

    // Initialise the window size from the terminal's implicit (configured) size, plus the title-bar chrome
    // height. The implicit size is only known once the first pane's display attaches its session and runs
    // updateImplicitSize(), which is asynchronous relative to Component.onCompleted — so apply it via a
    // one-shot on implicitWindowSizeChanged rather than a binding. A binding would re-evaluate on every later
    // implicit-size change (e.g. a DPR change) and snap the window back, overriding any WM-assigned geometry;
    // tiling WMs and manual resizes expect the window to keep its assigned size. _initialSizeApplied latches
    // the one-shot.
    property bool _initialSizeApplied: false
    Connections {
        target: terminalSessions
        function onImplicitWindowSizeChanged() {
            if (appWindow._initialSizeApplied)
                return
            var w = terminalSessions.implicitWindowWidth
            var h = terminalSessions.implicitWindowHeight
            if (w > 0 && h > 0) {
                appWindow.width = w
                appWindow.height = h + titleBar.effectiveHeight
                appWindow._initialSizeApplied = true
            }
        }
    }

    // {{{ Custom title bar + terminal content
    TitleBar {
        id: titleBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        visible: appWindow.showCustomTitleBar
        height: visible ? implicitHeight : 0
        property real effectiveHeight: visible ? implicitHeight : 0
        // Declared after the content area below, so a positive z keeps the bar above it. The terminal now
        // renders through a scene-graph node (TerminalRenderNode) and composites in z-order like any item,
        // so this is ordinary stacking — no longer a workaround for the terminal painting over siblings.
        z: 1
        controller: terminalSessions
        window: appWindow
        // Draw our own min/max/close controls only in frameless (CSD) mode. When the native frame is
        // shown (titleBarVisible), the OS draws those controls, so ours would duplicate them.
        useCustomWindowControls: !terminalSessions.titleBarVisible
    }

    // Content area: the active tab's pane tree is the SOLE renderer for every case — a single unsplit
    // terminal is just a single-leaf tree (one TerminalPane), and a split is a multi-leaf tree. There is no
    // separate single-pane display: this removes the vtui<->pane-tree session hand-off that caused a series
    // of split lifecycle bugs. The Loader is active whenever the active tab has a root pane (always, once the
    // startup session exists).
    Item {
        id: content
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        Loader {
            id: paneContentLoader
            anchors.fill: parent
            active: terminalSessions.activeTabRootPane !== null
            sourceComponent: PaneNode {
                node: terminalSessions.activeTabRootPane
            }
        }
    }

    // Frameless windows need their own edge/corner resize handles. With the native frame
    // (titleBarVisible), the WM already draws an edge resize border, so ours would duplicate it
    // and — being invisible z:1000 hit zones around every edge — would steal edge clicks (scrollbar
    // drags, text selection near a border) and start a system resize instead. Mirror the same
    // frameless gate WindowControls uses (TitleBar.useCustomWindowControls). visible:false also stops
    // hit-testing the MouseAreas, so the border is fully inert in native-frame mode.
    ResizeBorder {
        window: appWindow
        visible: !terminalSessions.titleBarVisible
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
        target: terminalSessions
        function onActiveTabIndexChanged() { appWindow.restoreTerminalFocus(); }
    }

    onClosing: {
        console.log("Terminal closed. Removing session.");
        terminalSessions.closeWindow();
    }

    function showNotification(title, content) {
        // "OSC 777 ; notify ; <TITLE> ; <CONTENT> ST"
        // Example: printf "\033]777;notify;Hello Title;Hello Content\033\\"
        console.log("main: notification [%1] %2".arg(title).arg(content));
        if (trayIcon.supportsMessages)
        {
            trayIcon.show();
            trayIcon.showMessage("Application Message: %1".arg(title),
                                 "%1".arg(content),
                                 60 * 1000);
        }
        else
            console.log("main: Notification system not supported!");
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
