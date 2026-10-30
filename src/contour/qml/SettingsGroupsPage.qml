// vim:syntax=qml
// A scrolling page of Settings groups: one SettingsGroup for each entry of `groups`, holding a SettingsRow for
// each of the `fields` whose `group` is that group's id. Groups with no field are not shown.
//
// What differs between the pages is only where `groups` and `fields` come from and what a commit does, so the
// section and profile pages are this with those filled in. `header` and `footer` take whatever else a page
// shows above and below its groups.
//
// The list is `settings.listMaxWidth` wide at most (0 fills the page) and centred.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Flickable {
    id: root

    /// The groups to show, in order: { id (a SettingsGroup), title }.
    property var groups: []
    /// The rows of the controller's lists the groups draw from.
    property var fields: []
    /// Whether the user can change them.
    property bool editable: true
    /// The families the font picker lists.
    property var fontFamilies: []
    /// What the page shows above its groups.
    property alias header: headerSlot.data
    /// What the page shows below its groups.
    property alias footer: footerSlot.data

    /// The user asked for @p value.
    signal committed(string key, var value)
    /// The user asked for the override of @p key to be dropped.
    signal resetRequested(string key)
    /// The user asked to choose the file of path field @p key.
    signal chooseRequested(string key)

    /// @return The fields of the group @p groupId.
    function fieldsOf(groupId) {
        return root.fields.filter(field => field.group === groupId);
    }

    /// @return The first item beneath @p item whose objectName is @p name, or null.
    function find(item, name) {
        for (const child of item.children) {
            if (child.objectName === name)
                return child;
            const found = root.find(child, name);
            if (found)
                return found;
        }
        return null;
    }

    /// Scrolls the row of the field @p key into view, and keeps it there while the page lays itself out: its
    /// groups and rows are built after the page is shown, so the row's place moves until they are. A scroll
    /// of the user's own lets go of it.
    /// @return Whether the page has such a row now.
    function revealRow(key) {
        root._revealed = key;
        return root.showRevealed();
    }

    // The field whose row is kept in view; empty for none.
    property string _revealed: ""

    /// Brings the revealed row into view.
    /// @return Whether the page has such a row.
    function showRevealed() {
        const row = root._revealed.length > 0 ? root.find(column, "settingsRow:" + root._revealed) : null;
        if (!row)
            return false;
        const top = row.mapToItem(root.contentItem, 0, 0).y;
        const room = chromeStyle.settings.groupGap;
        if (top < root.contentY)
            root.contentY = Math.max(0, top - room);
        else if (top + row.height > root.contentY + root.height)
            root.contentY = Math.max(0, Math.min(root.contentHeight - root.height,
                                                 top + row.height - root.height + room));
        return true;
    }

    onContentHeightChanged: root.showRevealed()
    onHeightChanged: root.showRevealed()
    onMovementStarted: root._revealed = ""

    readonly property real _listWidth: chromeStyle.settings.listMaxWidth > 0
                                       ? Math.min(chromeStyle.settings.listMaxWidth, root.width - 2 * chromeStyle.space.s8)
                                       : root.width - 2 * chromeStyle.space.s8

    clip: true
    contentWidth: width
    contentHeight: column.implicitHeight + 2 * chromeStyle.space.s8
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    ColumnLayout {
        id: column
        anchors.horizontalCenter: parent.horizontalCenter
        y: chromeStyle.space.s8
        width: root._listWidth
        spacing: chromeStyle.settings.groupGap

        ColumnLayout {
            id: headerSlot
            Layout.fillWidth: true
            spacing: chromeStyle.settings.groupGap
        }

        Repeater {
            model: root.groups

            delegate: SettingsGroup {
                id: group
                required property var modelData
                readonly property var groupFields: root.fieldsOf(group.modelData.id)
                Layout.fillWidth: true
                groupId: group.modelData.id
                title: group.modelData.title
                visible: group.groupFields.length > 0

                Repeater {
                    model: group.groupFields

                    delegate: SettingsRow {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        field: modelData
                        editable: root.editable
                        fontFamilies: root.fontFamilies
                        showSeparator: index < group.groupFields.length - 1
                        onCommitted: (key, value) => root.committed(key, value)
                        onResetRequested: key => root.resetRequested(key)
                        onChooseRequested: key => root.chooseRequested(key)
                    }
                }
            }
        }

        ColumnLayout {
            id: footerSlot
            Layout.fillWidth: true
            spacing: chromeStyle.settings.groupGap
        }
    }
}
