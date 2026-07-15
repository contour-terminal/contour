// vim:syntax=qml
// The in-app settings page, shown in the content area in place of the terminal when the
// WindowController's settingsActive flag is set (see main.qml). It binds entirely to a
// SettingsController (the editable bridge over the configuration) and a WindowController (to close the
// page). It never edits contour.yml: it creates/edits GUI-owned side files, and shows contour.yml /
// builtin entities read-only.
//
// Layout (Windows-Terminal style): a left navigation column (default-profile selector, the profile
// list, the color-scheme list) and a right content pane that edits the selected profile or scheme.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    // The SettingsController and the owning WindowController. Both null-guarded throughout: the
    // controllers are torn down before the QML tree on window close, and bindings re-evaluate once
    // against null during teardown (an unguarded access would raise a TypeError that fails the tests).
    property var controller: null
    property var windowController: null

    // Which editor the right pane shows: "profile", "scheme", or "" (nothing selected yet).
    property string editorMode: ""

    color: palette.window

    readonly property var profileNameList: {
        var out = []
        if (controller)
            for (var i = 0; i < controller.profiles.length; ++i)
                out.push(controller.profiles[i].name)
        return out
    }
    readonly property var schemeNameList: {
        var out = []
        if (controller)
            for (var i = 0; i < controller.colorSchemes.length; ++i)
                out.push(controller.colorSchemes[i].name)
        return out
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // {{{ Header
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Settings")
                font.pointSize: 18
                font.bold: true
                Layout.fillWidth: true
            }
            Label {
                objectName: "lockedBadge"
                visible: root.controller && root.controller.locked
                text: qsTr("read-only (gui_config_locked)")
                color: palette.mid
                verticalAlignment: Text.AlignVCenter
            }
            Button {
                objectName: "closeSettingsButton"
                text: qsTr("Close")
                onClicked: if (root.windowController) root.windowController.closeSettings()
            }
        }
        // }}}

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            // {{{ Left navigation
            ColumnLayout {
                Layout.preferredWidth: 260
                Layout.fillHeight: true
                spacing: 8

                Label { text: qsTr("Default profile"); font.bold: true }
                ComboBox {
                    id: defaultProfileCombo
                    objectName: "defaultProfileCombo"
                    Layout.fillWidth: true
                    model: root.profileNameList
                    enabled: root.controller && !root.controller.locked
                    currentIndex: root.controller ? root.profileNameList.indexOf(root.controller.defaultProfile) : -1
                    onActivated: if (root.controller) root.controller.setDefaultProfile(currentText)
                }

                Label { text: qsTr("Profiles"); font.bold: true; Layout.topMargin: 8 }
                Frame {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    padding: 2
                    ListView {
                        id: profileList
                        objectName: "profileList"
                        anchors.fill: parent
                        clip: true
                        model: root.controller ? root.controller.profiles : []
                        delegate: ItemDelegate {
                            required property var modelData
                            width: ListView.view.width
                            text: modelData.name + (modelData.isDefault ? "  ★" : "")
                                  + (modelData.origin === "side" ? "" : "  🔒")
                            onClicked: {
                                if (!root.controller) return
                                root.controller.editProfile(modelData.name)
                                root.editorMode = "profile"
                            }
                        }
                    }
                }
                Button {
                    objectName: "newProfileButton"
                    Layout.fillWidth: true
                    text: qsTr("New profile…")
                    enabled: root.controller && !root.controller.locked
                    onClicked: {
                        root.controller.newProfile(root.controller.defaultProfile)
                        root.editorMode = "profile"
                    }
                }

                Label { text: qsTr("Color schemes"); font.bold: true; Layout.topMargin: 8 }
                Frame {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 120
                    padding: 2
                    ListView {
                        id: schemeList
                        objectName: "schemeList"
                        anchors.fill: parent
                        clip: true
                        model: root.controller ? root.controller.colorSchemes : []
                        delegate: ItemDelegate {
                            required property var modelData
                            width: ListView.view.width
                            text: modelData.name + (modelData.editable ? "" : "  🔒")
                            onClicked: {
                                if (!root.controller) return
                                root.controller.editColorScheme(modelData.name)
                                root.editorMode = "scheme"
                            }
                        }
                    }
                }
                Button {
                    objectName: "newSchemeButton"
                    Layout.fillWidth: true
                    text: qsTr("New color scheme…")
                    enabled: root.controller && !root.controller.locked
                    onClicked: {
                        root.controller.newColorScheme("")
                        root.editorMode = "scheme"
                    }
                }
            }
            // }}}

            ToolSeparator { Layout.fillHeight: true }

            // {{{ Right content pane
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                // Profile editor.
                ColumnLayout {
                    width: parent.width
                    visible: root.editorMode === "profile"
                    spacing: 12

                    Label {
                        text: (root.controller && root.controller.editingProfile.length > 0)
                              ? qsTr("Profile: %1").arg(root.controller.editingProfile)
                              : qsTr("New profile")
                        font.pointSize: 14
                        font.bold: true
                    }
                    Label {
                        objectName: "readOnlyBanner"
                        visible: root.controller && root.controller.editingReadOnly
                        text: qsTr("This profile is defined in contour.yml and is read-only here. "
                                   + "Use \"Save As\" to create an editable copy.")
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        color: palette.mid
                    }

                    Repeater {
                        model: root.controller ? root.controller.profileFields : []
                        delegate: SettingRow {
                            required property var modelData
                            Layout.fillWidth: true
                            fieldKey: modelData.key
                            label: modelData.label
                            help: modelData.help
                            type: modelData.type
                            value: modelData.value
                            editable: root.controller && !root.controller.editingReadOnly
                            onEdited: (key, value) => root.controller.setProfileField(key, value)
                        }
                    }

                    // Color-scheme selection (with dark/light distinction).
                    Label { text: qsTr("Color scheme"); font.bold: true; Layout.topMargin: 8 }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label { text: qsTr("Mode") }
                        ComboBox {
                            id: schemeModeCombo
                            objectName: "schemeModeCombo"
                            enabled: root.controller && !root.controller.editingReadOnly
                            model: [qsTr("Single"), qsTr("Light / Dark")]
                            currentIndex: (root.controller && root.controller.colorSchemeMode === "dual") ? 1 : 0
                            onActivated: if (root.controller)
                                             root.controller.setColorSchemeMode(currentIndex === 1 ? "dual" : "simple")
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: root.controller && root.controller.colorSchemeMode === "simple"
                        spacing: 8
                        Label { text: qsTr("Scheme") }
                        ComboBox {
                            objectName: "simpleSchemeCombo"
                            Layout.preferredWidth: 220
                            enabled: root.controller && !root.controller.editingReadOnly
                            model: root.schemeNameList
                            currentIndex: root.controller ? root.schemeNameList.indexOf(root.controller.colorScheme) : -1
                            onActivated: if (root.controller) root.controller.setColorScheme(currentText)
                        }
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        visible: root.controller && root.controller.colorSchemeMode === "dual"
                        columns: 2
                        columnSpacing: 8
                        rowSpacing: 6
                        Label { text: qsTr("Light") }
                        ComboBox {
                            objectName: "lightSchemeCombo"
                            Layout.preferredWidth: 220
                            enabled: root.controller && !root.controller.editingReadOnly
                            model: root.schemeNameList
                            currentIndex: root.controller ? root.schemeNameList.indexOf(root.controller.colorSchemeLight) : -1
                            onActivated: if (root.controller) root.controller.setColorSchemeLight(currentText)
                        }
                        Label { text: qsTr("Dark") }
                        ComboBox {
                            objectName: "darkSchemeCombo"
                            Layout.preferredWidth: 220
                            enabled: root.controller && !root.controller.editingReadOnly
                            model: root.schemeNameList
                            currentIndex: root.controller ? root.schemeNameList.indexOf(root.controller.colorSchemeDark) : -1
                            onActivated: if (root.controller) root.controller.setColorSchemeDark(currentText)
                        }
                    }

                    // Save bar.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 12
                        spacing: 8
                        Button {
                            objectName: "saveProfileButton"
                            text: qsTr("Save")
                            enabled: root.controller && !root.controller.locked
                                     && !root.controller.editingReadOnly
                                     && root.controller.editingProfile.length > 0
                            onClicked: root.controller.saveProfile()
                        }
                        TextField {
                            id: saveAsField
                            objectName: "saveAsField"
                            Layout.preferredWidth: 180
                            placeholderText: qsTr("New profile name")
                            selectByMouse: true
                        }
                        Button {
                            objectName: "saveProfileAsButton"
                            text: qsTr("Save As")
                            enabled: root.controller && !root.controller.locked
                                     && saveAsField.text.trim().length > 0
                            onClicked: {
                                if (root.controller.saveProfileAs(saveAsField.text.trim()))
                                    saveAsField.clear()
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            objectName: "deleteProfileButton"
                            text: qsTr("Delete")
                            enabled: root.controller && !root.controller.locked
                                     && !root.controller.editingReadOnly
                                     && root.controller.editingProfile.length > 0
                            onClicked: root.controller.deleteProfile(root.controller.editingProfile)
                        }
                    }
                }

                // Color-scheme editor.
                ColorSchemeEditor {
                    width: parent.width
                    visible: root.editorMode === "scheme"
                    controller: root.controller
                }

                // Empty state.
                Label {
                    visible: root.editorMode === ""
                    text: qsTr("Select a profile or color scheme on the left, or create a new one.")
                    opacity: 0.7
                }
            }
            // }}}
        }
    }
}
