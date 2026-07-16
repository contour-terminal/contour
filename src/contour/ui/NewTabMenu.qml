// vim:syntax=qml
// The new-tab dropdown (the "▾" beside "+"): one entry per configured profile — picking one opens a new
// tab launched with that profile — then a separator and a "Settings" entry that opens the settings page.
// Windows-Terminal style.
//
// The profile list is data from the SettingsController (reached via the WindowController), so the menu
// never hard-codes it and stays in step with the config. The items are built at runtime the same way
// TerminalContextMenu builds its rows (createObject under contentItem + addItem), for the same reason:
// an Instantiator/Repeater cannot host a heterogeneous Menu, and a native OS menu cannot host runtime
// items (it came up empty on Windows) — so this forces the in-scene popup and builds by hand.
import QtQuick
import QtQuick.Controls

Menu {
    id: root

    // Force the in-scene (item) popup rather than a native OS menu (see TerminalContextMenu for the why).
    popupType: Popup.Item

    // The WindowController. Null-guarded throughout: it is torn down before this QML tree on window close.
    required property var controller

    // Objects this file created, destroyed on the next rebuild so nothing leaks.
    property var _created: []

    // Build the items when the component is complete (so they exist for offscreen tests, which cannot
    // open a popup) and again just before each open (so a profile added by a live config reload shows up
    // without reopening the window).
    Component.onCompleted: root.rebuild()
    onAboutToShow: root.rebuild()

    Component {
        id: profileEntry
        MenuItem {
            property string profileName: ""
            onTriggered: if (root.controller) root.controller.createNewTab(profileName)
        }
    }
    Component {
        id: separatorEntry
        MenuSeparator {}
    }
    Component {
        id: settingsEntry
        MenuItem {
            text: qsTr("Settings")
            onTriggered: if (root.controller) root.controller.openSettings()
        }
    }

    // The configured profiles, or [] when unreachable (e.g. a lightweight mock controller in the tests,
    // which has no settingsController). Reading a missing QObject property yields undefined, not an error.
    function profileList() {
        if (root.controller && root.controller.settingsController
                && root.controller.settingsController.profiles)
            return root.controller.settingsController.profiles
        return []
    }

    function rebuild() {
        // Tear down what we built last time (Qt owns nothing created here).
        while (root.count > 0)
            root.takeItem(0)
        for (let i = 0; i < root._created.length; ++i)
            root._created[i].destroy()
        root._created = []

        let created = []
        const profiles = root.profileList()
        for (let i = 0; i < profiles.length; ++i) {
            const item = profileEntry.createObject(root.contentItem, {
                "text": qsTr("New tab — %1").arg(profiles[i].name),
                "profileName": profiles[i].name
            })
            root.addItem(item)
            created.push(item)
        }
        if (profiles.length > 0) {
            const sep = separatorEntry.createObject(root.contentItem)
            root.addItem(sep)
            created.push(sep)
        }
        const settings = settingsEntry.createObject(root.contentItem)
        root.addItem(settings)
        created.push(settings)

        root._created = created
    }
}
