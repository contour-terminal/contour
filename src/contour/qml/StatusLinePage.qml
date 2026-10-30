// vim:syntax=qml
// A profile's Status Line page: the live preview of the profile with its status line shown, the profile's
// status-line fields, and the indicator's three segments as lists of placeholders to add, style, order and
// remove.
//
// The controller is the single source of truth: the fields commit through commitProfileField, and every
// segment edit goes through StatusLineIndicatorEditor.commit, which stages the draft the preview follows. The
// controller refuses a read-only or locked profile; `editable` here is a courtesy on top of that boundary.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

SettingsGroupsPage {
    id: root
    objectName: "statusLinePage"

    /// The SettingsController.
    property var controller: null
    /// The window's SettingsPreview, whose terminal the hero shows; null where the host has none.
    property var preview: null
    /// The profile the page shows.
    property string profileName: ""

    groups: {
        const profiles = root.controller ? root.controller.sections.find(row => row.id === "profiles") : null;
        return profiles ? profiles.groups.filter(group => group.scope === "profile"
                                                           && group.id === root.controller.statusLineGroup) : [];
    }
    fields: root.controller ? root.controller.profileFields : []
    editable: root.controller !== null && !root.controller.locked && !root.controller.editingReadOnly
    onCommitted: (key, value) => root.controller.commitProfileField(key, value)

    header: [
        SettingsPreview {
            Layout.fillWidth: true
            preview: root.preview
            profileName: root.profileName
            statusLine: true
        }
    ]

    footer: [
        SettingsGroup {
            objectName: "statusLineSegmentsGroup"
            Layout.fillWidth: true
            title: qsTr("Indicator segments")

            StatusLineIndicatorEditor {
                Layout.fillWidth: true
                Layout.margins: chromeStyle.settings.rowPadding
                controller: root.controller
                editable: root.editable
            }
        }
    ]
}
