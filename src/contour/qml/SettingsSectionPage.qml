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
    /// A link to a page of its own was asked to open; @p target is the link's id.
    signal linkRequested(string target)

    readonly property var _section: root.controller ? root.controller.sections.find(row => row.id === root.sectionId) : null

    readonly property var _links: root._section ? root._section.links : []

    groups: root._section ? root._section.groups.filter(group => group.scope === "global") : []
    fields: root.controller ? root.controller.globalFields : []
    editable: root.controller !== null && !root.controller.locked
    fontFamilies: root.controller ? root.controller.fontFamilies : []
    onCommitted: (key, value) => root.controller.commitGlobalField(key, value)
    onResetRequested: key => root.controller.commitGlobalReset(key)

    // The Shell Integration section's Install rows: one per shell, what its startup file is, and an Install…
    // that asks before adding the line.
    readonly property var _shellRows: root.sectionId === "shell-integration" && root.controller
                                      ? root.controller.shellIntegrationRows : []

    /// Asks whether to add the line that loads Contour's integration to @p shell's startup file.
    function askInstall(shell) {
        installDialog.shell = shell;
        installDialog.confirm = root.controller.shellIntegrationConfirm(shell);
        installDialog.open();
    }

    KitDialog {
        id: installDialog
        objectName: "shellInstallDialog"
        /// The config::ShellKind the dialog is about.
        property int shell: -1
        /// What the question says: { title, line, primary, secondary }.
        property var confirm: ({})
        heading: installDialog.confirm.title !== undefined ? installDialog.confirm.title : ""
        acceptText: installDialog.confirm.primary !== undefined ? installDialog.confirm.primary : ""
        rejectText: installDialog.confirm.secondary !== undefined ? installDialog.confirm.secondary : ""
        description: installDialog.heading
        onAccepted: if (root.controller) root.controller.installShellIntegration(installDialog.shell)
        bodyData: [
            // The line itself, as it will be written, in the terminal's font: what is added is read before it
            // is.
            Label {
                objectName: "shellInstallLine"
                Layout.fillWidth: true
                text: installDialog.confirm.line !== undefined ? installDialog.confirm.line : ""
                font.family: root.controller && root.controller.fontFamilies.length > 0
                             ? root.controller.fontFamilies[0] : chromeStyle.type.body.family
                font.pixelSize: chromeStyle.type.footnote.pixelSize
                color: chromeStyle.colors.settingsLabel
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText
            }
        ]
    }

    header: [
        // What each shell's startup file holds, and the way to add Contour's integration to it.
        SettingsGroup {
            objectName: "shellInstallGroup"
            Layout.fillWidth: true
            visible: root._shellRows.length > 0
            title: qsTr("Install")

            Repeater {
                model: root._shellRows

                delegate: Item {
                    id: shellRow
                    required property var modelData
                    required property int index
                    objectName: "shellInstallRow:" + shellRow.modelData.name
                    Layout.fillWidth: true
                    implicitHeight: Math.max(chromeStyle.settings.row,
                                             shellTexts.implicitHeight + 2 * chromeStyle.space.s4)

                    Accessible.role: Accessible.ListItem
                    Accessible.name: shellRow.modelData.name + ", " + shellRow.modelData.status

                    // Under the row, where the group is one card.
                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: chromeStyle.settings.rowPadding
                        height: chromeStyle.borderWidth
                        visible: shellRow.index < root._shellRows.length - 1
                                 && chromeStyle.settingsTraits.rows === chromeStyle.settingsTraits.grouped
                        color: chromeStyle.colors.settingsSeparator
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: chromeStyle.settings.rowPadding
                        anchors.rightMargin: chromeStyle.settings.rowPadding
                        spacing: chromeStyle.space.s6

                        ColumnLayout {
                            id: shellTexts
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 0

                            Label {
                                objectName: "shellInstallName"
                                Layout.fillWidth: true
                                text: shellRow.modelData.name
                                font: chromeStyle.type.body
                                color: chromeStyle.colors.settingsLabel
                                textFormat: Text.PlainText
                                Accessible.ignored: true
                            }
                            Label {
                                objectName: "shellInstallStatus"
                                Layout.fillWidth: true
                                text: shellRow.modelData.status
                                font: chromeStyle.type.footnote
                                color: chromeStyle.colors.settingsHelp
                                wrapMode: Text.Wrap
                                textFormat: Text.PlainText
                                Accessible.ignored: true
                            }
                            // A cut-short install: the very line to delete, as the file holds it.
                            Label {
                                objectName: "shellInstallDeleteLine"
                                Layout.fillWidth: true
                                visible: shellRow.modelData.deleteLine.length > 0
                                text: shellRow.modelData.deleteLine
                                font.family: root.controller && root.controller.fontFamilies.length > 0
                                             ? root.controller.fontFamilies[0] : chromeStyle.type.body.family
                                font.pixelSize: chromeStyle.type.footnote.pixelSize
                                color: chromeStyle.colors.settingsLabel
                                wrapMode: Text.WrapAnywhere
                                textFormat: Text.PlainText
                                Accessible.name: qsTr("Line to delete: %1").arg(text)
                            }
                        }

                        Button {
                            objectName: "shellInstallButton:" + shellRow.modelData.name
                            Layout.alignment: Qt.AlignVCenter
                            visible: shellRow.modelData.canInstall === true
                            enabled: root.editable
                            flat: true
                            text: qsTr("Install…")
                            font: chromeStyle.type.footnote
                            palette.buttonText: chromeStyle.colors.settingsLabel
                            Accessible.name: qsTr("Install shell integration for %1").arg(shellRow.modelData.name)
                            onClicked: root.askInstall(shellRow.modelData.shell)
                        }
                    }
                }
            }
        }
    ]

    footer: [
        // The pages of their own this section links to.
        SettingsGroup {
            objectName: "sectionLinksGroup"
            Layout.fillWidth: true
            visible: root._links.length > 0

            Repeater {
                model: root._links

                delegate: Control {
                    id: link
                    required property var modelData
                    objectName: "sectionLink:" + link.modelData.target
                    Layout.fillWidth: true
                    implicitHeight: chromeStyle.settings.row
                    activeFocusOnTab: true

                    Accessible.role: Accessible.Button
                    Accessible.name: link.modelData.title
                    Accessible.onPressAction: root.linkRequested(link.modelData.target)

                    Keys.onReturnPressed: root.linkRequested(link.modelData.target)
                    Keys.onSpacePressed: root.linkRequested(link.modelData.target)

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
                        onTapped: root.linkRequested(link.modelData.target)
                    }
                }
            }
        },

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
