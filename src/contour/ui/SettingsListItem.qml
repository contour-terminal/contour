// vim:syntax=qml
// One row in the settings page's profile or color-scheme list. Beyond selecting the entry (a single
// click opens it in the editor on the right), the row carries its per-entry actions inline, in the
// Windows-Terminal idiom:
//   - a home icon that marks and sets the DEFAULT profile (profiles only, gated by `showHome`); the
//     default wears a filled home, the others reveal a faint one on hover — click it to move the default;
//   - inline RENAME: double-click the name (or the context menu) to edit it in place, Enter commits and
//     Escape/blur cancels — the same gesture the tab strip uses for tab titles;
//   - a red TRASHCAN on hover to DELETE the entry (the page routes it through a confirmation dialog);
//   - a right-click CONTEXT MENU gathering Set-as-default / Rename / Delete.
// Rename and delete are offered only for `editable` (GUI-created side-file) entries; the default may
// point at any profile. Colours follow the OS light/dark theme via SystemPalette.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string entryName: ""
    property bool selected: false
    property bool editable: true   ///< A GUI side-file entry (rename/delete allowed).
    property bool isDefault: false ///< This profile is the current default (profiles only).
    property bool showHome: false  ///< Show the default (home) affordance (profiles yes, schemes no).
    property bool locked: false    ///< The whole settings page is read-only.

    signal activated()                        ///< Open this entry in the editor.
    signal setDefaultRequested()              ///< Make this profile the default.
    signal renameRequested(string newName)    ///< Commit an inline rename.
    signal deleteRequested()                  ///< Delete this entry (host shows the confirmation dialog).

    implicitHeight: 38

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }

    HoverHandler { id: rowHover }

    // Selected pill + accent wash on hover (mirrors SettingsNavItem).
    Rectangle {
        anchors.fill: parent
        radius: 6
        visible: root.selected
        color: sys.base
    }
    Rectangle {
        anchors.fill: parent
        radius: 6
        color: sys.highlight
        opacity: root.selected ? 0.16 : (rowHover.hovered ? 0.08 : 0.0)
        Behavior on opacity { NumberAnimation { duration: 90 } }
    }
    Rectangle {
        visible: root.selected
        width: 3
        radius: 1.5
        color: sys.highlight
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom; topMargin: 7; bottomMargin: 7 }
    }

    // Single click opens the entry; a double-click also starts an inline rename (editable only). Mirrors
    // TabItem.qml's tap handling.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: {
            root.activated();
            if (tapCount === 2 && root.editable && !root.locked)
                root.startRename();
        }
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: contextMenu.popup()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 6
        spacing: 6

        // Home / default affordance (profiles only): filled for the default, a faint outline on hover for
        // the others — click to make that profile the default.
        AbstractButton {
            id: homeButton
            objectName: "setDefaultButton"
            visible: root.showHome
            implicitWidth: 20
            implicitHeight: 20
            Layout.alignment: Qt.AlignVCenter
            enabled: !root.locked && !root.isDefault
            opacity: root.isDefault ? 1.0 : (rowHover.hovered ? 0.55 : 0.0)
            Behavior on opacity { NumberAnimation { duration: 90 } }
            ToolTip.visible: hovered
            ToolTip.text: root.isDefault ? qsTr("Default profile") : qsTr("Set as default")
            onClicked: if (!root.isDefault) root.setDefaultRequested()
            contentItem: Label {
                text: "⌂"
                font.pointSize: 11
                color: root.isDefault ? sys.highlight : sys.windowText
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: null
        }

        // The name, or an inline rename editor in its place.
        Label {
            id: nameLabel
            visible: !renameField.visible
            text: root.entryName
            color: sys.windowText
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            font.weight: root.selected ? Font.DemiBold : Font.Normal
        }
        TextField {
            id: renameField
            objectName: "renameField"
            visible: false
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            selectByMouse: true
            onAccepted: root.commitRename()
            onActiveFocusChanged: if (!activeFocus && visible) root.cancelRename()
            Keys.onEscapePressed: root.cancelRename()
        }

        // Delete (trashcan) — editable rows only, revealed on hover / when selected.
        AbstractButton {
            id: trashButton
            objectName: "deleteEntryButton"
            visible: root.editable && !root.locked && (rowHover.hovered || root.selected)
            implicitWidth: 22
            implicitHeight: 22
            Layout.alignment: Qt.AlignVCenter
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Delete")
            onClicked: root.deleteRequested()
            contentItem: Label {
                text: "🗑"
                font.pointSize: 11
                color: trashButton.hovered ? "#e53935" : "#c62828"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 4
                color: trashButton.hovered ? Qt.rgba(0.9, 0.2, 0.2, 0.16) : "transparent"
            }
        }
    }

    Menu {
        id: contextMenu
        popupType: Popup.Item
        MenuItem {
            text: qsTr("Set as default")
            height: visible ? implicitHeight : 0
            visible: root.showHome
            enabled: root.showHome && !root.isDefault && !root.locked
            onTriggered: root.setDefaultRequested()
        }
        MenuItem {
            text: qsTr("Rename")
            enabled: root.editable && !root.locked
            onTriggered: root.startRename()
        }
        MenuItem {
            text: qsTr("Delete")
            enabled: root.editable && !root.locked
            onTriggered: root.deleteRequested()
        }
    }

    function startRename() {
        renameField.text = root.entryName;
        renameField.visible = true;
        renameField.forceActiveFocus();
        renameField.selectAll();
    }
    function commitRename() {
        var name = renameField.text.trim();
        renameField.visible = false;
        if (name.length > 0 && name !== root.entryName)
            root.renameRequested(name);
    }
    function cancelRename() {
        renameField.visible = false;
    }
}
