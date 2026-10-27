// vim:syntax=qml
// A titled group of Settings rows. In the `grouped` row shape the rows are one card with a separator between
// them; in `separateCards` each row draws its own card and the gap between them is `settings.cardGap`.
//
// The rows are its children: whatever is declared inside is laid out one beneath the other.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    /// The group's title, shown above its rows.
    property string title
    /// The group's id, a SettingsGroup enumerator; names the group "settingsGroup:<id>".
    property int groupId: -1
    /// The rows.
    default property alias rows: rowsColumn.data

    readonly property bool _grouped: chromeStyle.settingsTraits.rows === chromeStyle.settingsTraits.grouped

    objectName: "settingsGroup:" + root.groupId
    spacing: chromeStyle.settings.groupTitleGap

    Label {
        objectName: "settingsGroupTitle"
        Layout.fillWidth: true
        Layout.leftMargin: chromeStyle.settings.rowPadding
        visible: root.title.length > 0
        text: root.title
        font: chromeStyle.settings.groupTitleType > 0
              ? Qt.font({ family: chromeStyle.type.footnoteStrong.family,
                          weight: chromeStyle.type.footnoteStrong.weight,
                          pixelSize: chromeStyle.settings.groupTitleType })
              : chromeStyle.type.footnoteStrong
        color: chromeStyle.colors.settingsHelp
        textFormat: Text.PlainText

        Accessible.role: Accessible.Heading
        Accessible.name: root.title
    }

    Item {
        Layout.fillWidth: true
        implicitHeight: rowsColumn.implicitHeight

        Rectangle {
            anchors.fill: parent
            visible: root._grouped
            radius: chromeStyle.settings.groupRadius
            color: chromeStyle.colors.settingsGroupFill
        }

        ColumnLayout {
            id: rowsColumn
            anchors.fill: parent
            spacing: root._grouped ? 0 : chromeStyle.settings.cardGap
        }
    }
}
