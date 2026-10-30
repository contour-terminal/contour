// vim:syntax=qml
// The app's one Settings window: the settings page in a top-level window of its own, built once by
// SettingsWindowHost (window/SettingsWindowHost.hpp). Closing it closes the platform window but keeps this
// object, so the pane and the draft it shows are where the user left them.
//
// It has no width/height binding on purpose: the host places and sizes it before its first show, from
// preferredSize and the minimum below, and after that the size is the user's. A republished chromeStyle
// re-evaluates the minimum only.
import QtQuick
import QtQuick.Controls

ThemedWindow
{
    id: settingsWindow
    objectName: "settingsWindow"

    // The SettingsWindowHost: the controller the page edits, the title and title-bar buttons of the host
    // platform's conventions, and closeSettings().
    required property var host

    visible: false
    title: host ? host.title : ""
    flags: host ? host.windowFlags : Qt.Window
    // No `color:` binding: an ApplicationWindow keeps its colour and palette.window in step already, and
    // binding one to the other is a binding loop. ThemedWindow's palette paints the background.

    minimumWidth: chromeStyle.size.settingsMinWidth
    minimumHeight: chromeStyle.size.settingsMinHeight

    // The size the host gives the window on its first show; read from C++, never bound to width/height.
    readonly property size preferredSize: Qt.size(chromeStyle.size.settingsWidth, chromeStyle.size.settingsHeight)

    // Whether the window draws its own frame, beside a Linux header bar (R-l): then it resizes from its own
    // edges, its header row is its title bar, and the desktop's close button sits at that row's end.
    readonly property bool frameless: !!host && host.frameless

    // What WindowControls and the header row ask of a window's controller, for this window: the terminal
    // window's goes through WindowController, which keeps its size increments; this one has none to keep, and
    // no caption the OS drives.
    QtObject {
        id: frameController
        readonly property bool nativeMaximizeHovered: false
        readonly property bool nativeMaximizePressed: false
        function minimizeWindow() { settingsWindow.showMinimized() }
        function toggleMaximized() {
            if (settingsWindow.visibility === Window.Maximized)
                settingsWindow.showNormal()
            else
                settingsWindow.showMaximized()
        }
        function setCaptionButtonRect(owner, kind, x, y, width, height) {}
        function clearCaptionButtonRect(owner, kind) {}
    }

    Component {
        id: leadingControlsComponent

        WindowControls {
            side: "leading"
            window: settingsWindow
            controller: frameController
            kinds: settingsWindow.host.leadingControls
        }
    }
    Component {
        id: trailingControlsComponent

        WindowControls {
            side: "trailing"
            window: settingsWindow
            controller: frameController
            kinds: settingsWindow.host.trailingControls
        }
    }

    SettingsPage
    {
        id: settingsPage
        objectName: "settingsPage"
        anchors.fill: parent
        focus: true
        host: settingsWindow.host
        windowController: settingsWindow.frameless ? frameController : null
        leadingControls: settingsWindow.frameless && settingsWindow.host.leadingControls.length > 0
                         ? leadingControlsComponent : null
        trailingControls: settingsWindow.frameless && settingsWindow.host.trailingControls.length > 0
                          ? trailingControlsComponent : null

        // The page the host remembers across restarts: opened on it, and told of every change. A page the
        // page does not know -- one an older or newer release named, or a profile or scheme that is gone --
        // leaves it on its first page.
        // Written back only once restored: the page's first page settles while it is built, before it has
        // read the one the host remembers.
        property bool pageRestored: false
        Component.onCompleted: {
            if (settingsWindow.host)
                settingsPage.openPage(settingsWindow.host.lastPage)
            pageRestored = true
        }
        onCurrentPageChanged: if (pageRestored && settingsWindow.host) settingsWindow.host.lastPage = settingsPage.currentPage
    }

    // A page another part of the app asks for (the Connect picker's Manage Hosts…, Edit Host…) while the window
    // is already built; a window not built yet reads it as the remembered page.
    Connections {
        target: settingsWindow.host
        function onPageRequested(page) {
            settingsPage.openPage(page)
        }
    }

    ResizeBorder {
        window: settingsWindow
        visible: settingsWindow.frameless
    }

    Shortcut
    {
        objectName: "closeSettingsShortcut"
        sequences: [StandardKey.Close]
        context: Qt.WindowShortcut
        onActivated: if (settingsWindow.host) settingsWindow.host.closeSettings()
    }
}
