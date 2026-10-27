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

    SettingsPage
    {
        id: settingsPage
        objectName: "settingsPage"
        anchors.fill: parent
        focus: true
        controller: settingsWindow.host ? settingsWindow.host.controller : null
        windowController: settingsWindow.host

        // The page the host remembers across restarts: opened on it, and told of every change. A page the
        // page does not know -- one an older or newer release named -- leaves it on its first page.
        Component.onCompleted: {
            if (settingsWindow.host && settingsPage.pages.indexOf(settingsWindow.host.lastPage) >= 0)
                settingsPage.editorMode = settingsWindow.host.lastPage
        }
        onEditorModeChanged: if (settingsWindow.host) settingsWindow.host.lastPage = settingsPage.editorMode
    }

    Shortcut
    {
        objectName: "closeSettingsShortcut"
        sequences: [StandardKey.Close]
        context: Qt.WindowShortcut
        onActivated: if (settingsWindow.host) settingsWindow.host.closeSettings()
    }
}
