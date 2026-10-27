// vim:syntax=qml
// A profile's page: the groups of its settings, from `controller.profileFields`, with the pages of what is
// not a scalar -- the colour scheme, the link patterns and the status line -- one row away.
//
// What it edits is the controller's profile draft, which the page opens (`controller.editProfile`) before it
// is shown. A profile defined in contour.yml or built in is shown read-only: its controls are disabled.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

SettingsGroupsPage {
    id: root
    objectName: "profilePage"

    /// The SettingsController.
    property var controller: null

    /// A sub-page was asked to open: "schemes", "links" or "statusline".
    signal subpageRequested(string kind)

    /// The sub-pages of a profile: the kind in the page id, and what its row reads.
    readonly property var subpages: [
        { kind: "schemes", title: qsTr("Colors") },
        { kind: "links", title: qsTr("Links") },
        { kind: "statusline", title: qsTr("Status Line") }
    ]

    groups: {
        const profiles = root.controller ? root.controller.sections.find(row => row.id === "profiles") : null;
        return profiles ? profiles.groups.filter(group => group.scope === "profile") : [];
    }
    fields: root.controller ? root.controller.profileFields : []
    editable: root.controller !== null && !root.controller.locked && !root.controller.editingReadOnly
    fontFamilies: root.controller ? root.controller.fontFamilies : []
    onCommitted: (key, value) => root.controller.commitProfileField(key, value)

    header: [
        SettingsGroup {
            objectName: "profileSubpagesGroup"
            Layout.fillWidth: true

            Repeater {
                model: root.subpages

                delegate: Control {
                    id: link
                    required property var modelData
                    objectName: "profileSubpage:" + link.modelData.kind
                    Layout.fillWidth: true
                    implicitHeight: chromeStyle.settings.row
                    activeFocusOnTab: true

                    Accessible.role: Accessible.Button
                    Accessible.name: link.modelData.title
                    Accessible.onPressAction: root.subpageRequested(link.modelData.kind)

                    Keys.onReturnPressed: root.subpageRequested(link.modelData.kind)
                    Keys.onSpacePressed: root.subpageRequested(link.modelData.kind)

                    contentItem: RowLayout {
                        spacing: chromeStyle.space.s5

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: chromeStyle.settings.rowPadding
                            text: link.modelData.title
                            font: chromeStyle.type.body
                            color: chromeStyle.colors.settingsLabel
                            textFormat: Text.PlainText
                            Accessible.ignored: true
                        }
                        Label {
                            Layout.rightMargin: chromeStyle.settings.rowPadding
                            text: "›"
                            font: chromeStyle.type.body
                            color: chromeStyle.colors.settingsHelp
                            Accessible.ignored: true
                        }
                    }
                    TapHandler {
                        onTapped: root.subpageRequested(link.modelData.kind)
                    }
                }
            }
        }
    ]
}
