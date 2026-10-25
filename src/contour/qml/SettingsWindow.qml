// vim:syntax=qml
// The app's one Settings window: the settings page in a top-level window of its own, built once by
// SettingsWindowHost (window/SettingsWindowHost.hpp) and hidden, never destroyed, when it closes, so the
// pane and the draft it shows are where the user left them.
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
        objectName: "settingsPage"
        anchors.fill: parent
        focus: true
        controller: settingsWindow.host ? settingsWindow.host.controller : null
        windowController: settingsWindow.host
    }

    Shortcut
    {
        objectName: "closeSettingsShortcut"
        sequences: [StandardKey.Close]
        context: Qt.WindowShortcut
        onActivated: if (settingsWindow.host) settingsWindow.host.closeSettings()
    }
}
