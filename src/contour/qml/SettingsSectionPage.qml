// vim:syntax=qml
// A section's page: the groups of application-wide settings the section holds, from `controller.globalFields`,
// edited as overrides in settings.yml. The Keyboard section also lists the key bindings, which are read-only
// here; the Profiles section lists the profiles, each opening its own page.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

SettingsGroupsPage {
    id: root
    objectName: "settingsSectionPage"

    /// The SettingsController.
    property var controller: null
    /// The section's id, as `controller.sections` names it.
    property string sectionId

    /// A profile row was asked to open its page.
    signal profileRequested(string name)

    readonly property var _section: root.controller ? root.controller.sections.find(row => row.id === root.sectionId) : null

    groups: root._section ? root._section.groups.filter(group => group.scope === "global") : []
    fields: root.controller ? root.controller.globalFields : []
    editable: root.controller !== null && !root.controller.locked
    fontFamilies: root.controller ? root.controller.fontFamilies : []
    onCommitted: (key, value) => root.controller.commitGlobalField(key, value)
    onResetRequested: key => root.controller.commitGlobalReset(key)

    footer: [
        // The key bindings: the configured ones, then the built-in fallbacks they leave free.
        SettingsGroup {
            objectName: "keybindingsGroup"
            Layout.fillWidth: true
            visible: root.sectionId === "keyboard"
            title: qsTr("Key bindings")

            Repeater {
                objectName: "keybindingsList"
                model: root.sectionId === "keyboard" && root.controller ? root.controller.keybindings : []

                delegate: Item {
                    id: binding
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: chromeStyle.settings.row

                    Accessible.role: Accessible.ListItem
                    Accessible.name: binding.modelData.trigger + " " + binding.modelData.action

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: chromeStyle.settings.rowPadding
                        anchors.rightMargin: chromeStyle.settings.rowPadding
                        spacing: chromeStyle.space.s6

                        Label {
                            Layout.preferredWidth: chromeStyle.settings.sidebarWidth
                            text: binding.modelData.trigger
                            font: chromeStyle.type.shortcut
                            color: chromeStyle.colors.settingsLabel
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            Accessible.ignored: true
                        }
                        Label {
                            Layout.fillWidth: true
                            text: binding.modelData.action
                            font: chromeStyle.type.body
                            color: chromeStyle.colors.settingsLabel
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            Accessible.ignored: true
                        }
                        Label {
                            text: binding.modelData.mode
                            font: chromeStyle.type.footnote
                            color: chromeStyle.colors.settingsHelp
                            Accessible.ignored: true
                        }
                        // A binding no input_mapping row lists: Contour's own, in effect until contour.yml
                        // binds the same chord.
                        Label {
                            visible: binding.modelData.builtIn === true
                            text: qsTr("built-in")
                            font: chromeStyle.type.footnote
                            color: chromeStyle.colors.settingsHelp
                            Accessible.ignored: true
                        }
                    }
                }
            }
        },

        // The profiles, each opening its own page.
        SettingsGroup {
            objectName: "profilesGroup"
            Layout.fillWidth: true
            visible: root.sectionId === "profiles"
            title: qsTr("Profiles")

            Repeater {
                model: root.sectionId === "profiles" && root.controller ? root.controller.profiles : []

                delegate: Control {
                    id: profile
                    required property var modelData
                    objectName: "profileOverviewRow:" + profile.modelData.name
                    Layout.fillWidth: true
                    implicitHeight: chromeStyle.settings.row
                    activeFocusOnTab: true

                    Accessible.role: Accessible.Button
                    Accessible.name: profile.modelData.name
                    Accessible.onPressAction: root.profileRequested(profile.modelData.name)

                    Keys.onReturnPressed: root.profileRequested(profile.modelData.name)
                    Keys.onSpacePressed: root.profileRequested(profile.modelData.name)

                    contentItem: Label {
                        leftPadding: chromeStyle.settings.rowPadding
                        verticalAlignment: Text.AlignVCenter
                        text: profile.modelData.isDefault ? qsTr("%1 (default)").arg(profile.modelData.name)
                                                          : profile.modelData.name
                        font: chromeStyle.type.body
                        color: chromeStyle.colors.settingsLabel
                        textFormat: Text.PlainText
                        Accessible.ignored: true
                    }
                    TapHandler {
                        onTapped: root.profileRequested(profile.modelData.name)
                    }
                }
            }
        }
    ]
}
