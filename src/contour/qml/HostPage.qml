// vim:syntax=qml
// A saved host's page: its rows from `controller.hostFields`, grouped as the Hosts section's groups are, those
// of the transport its address names. What is wrong with a row is said under it, in the loader's words, and a
// page with a problem saves nothing.
//
// What it edits is the controller's host draft, which the page opens (`controller.editHost`) before it is
// shown. A host defined in contour.yml is shown read-only: its controls are disabled.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

SettingsGroupsPage {
    id: root
    objectName: "hostPage"

    /// The SettingsController.
    property var controller: null
    /// The host the page shows.
    property string hostName: ""

    readonly property var _row: root.controller ? root.controller.hosts.find(row => row.name === root.hostName) : null

    groups: {
        const hosts = root.controller ? root.controller.sections.find(row => row.id === "hosts") : null;
        return hosts ? hosts.groups.filter(group => group.scope === "host") : [];
    }
    fields: root.controller ? root.controller.hostFields : []
    editable: root.controller !== null && !root.controller.locked && !root.controller.editingHostReadOnly
    onCommitted: (key, value) => root.controller.commitHostField(key, value)
    onChooseRequested: key => root.controller.chooseHostFile(key)

    footer: [
        // Where the host is kept, and where else it is listed.
        Label {
            objectName: "hostSavedNote"
            Layout.fillWidth: true
            Layout.leftMargin: chromeStyle.settings.rowPadding
            Layout.rightMargin: chromeStyle.settings.rowPadding
            visible: root.editable && root._row !== null && root._row !== undefined
            text: root._row ? qsTr("Saved to %1 beside contour.yml. Also listed under Attach to Host in the palette, the terminal's context menu and the title-bar menu.").arg(root._row.file)
                            : ""
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.settingsHelp
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }
    ]
}
