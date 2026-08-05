// vim:syntax=qml
// The color-scheme editor: a scrollable grid of swatch cards (default fg/bg + the 8 normal and 8
// bright ANSI colors) bound to SettingsController.schemeColors ({ key, label, color }). Each card
// shows a live swatch and an editable "#rrggbb" field; committing writes the slot back through
// SettingsController.setSchemeColor. A pinned footer bar carries the scheme name + Save / Delete.
//
// A hex field is used deliberately instead of a native ColorDialog: QtQuick.Dialogs is not guaranteed
// present on every target, and a hard import of it would break page loading where it is absent.
//
// Dark/light is handled at the PROFILE level (a profile references two schemes); a single scheme is one
// palette, edited here, and either referenced directly or picked as the light or dark half of a profile.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    // The SettingsController (may be null during teardown; every access below is guarded).
    property var controller: null

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
    readonly property color subtleText: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.62)
    readonly property color hairline: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.12)

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            text: qsTr("Edit the palette below. A profile can reference this scheme directly, or pick it "
                       + "as its light or dark half.")
            color: root.subtleText
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            GridLayout {
                width: scroll.availableWidth
                columns: 2
                columnSpacing: 12
                rowSpacing: 10

                Repeater {
                    model: root.controller ? root.controller.schemeColors : []
                    delegate: Item {
                        id: slot
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 56

                        Rectangle {
                            anchors.fill: parent
                            radius: 8
                            color: sys.base
                            border.width: 1
                            border.color: root.hairline
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 12

                            Rectangle {
                                width: 32
                                height: 32
                                radius: 6
                                color: slot.modelData.color
                                border.color: root.hairline
                                border.width: 1
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Label {
                                text: slot.modelData.label
                                color: sys.windowText
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                            }
                            TextField {
                                Layout.preferredWidth: 110
                                Layout.alignment: Qt.AlignVCenter
                                text: slot.modelData.color
                                enabled: root.controller && !root.controller.locked
                                selectByMouse: true
                                font.family: "monospace"
                                inputMask: "\\#HHHHHH"
                                onEditingFinished: if (root.controller)
                                                       root.controller.setSchemeColor(slot.modelData.key, text)
                            }
                        }
                    }
                }
            }
        }

        // Save / delete bar for the scheme, pinned below the scrolling palette.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: footerRow.implicitHeight + 20
            radius: 8
            color: sys.base
            border.width: 1
            border.color: root.hairline

            RowLayout {
                id: footerRow
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                TextField {
                    id: schemeNameField
                    objectName: "schemeNameField"
                    Layout.preferredWidth: 200
                    placeholderText: qsTr("Scheme name")
                    // The initial value; a one-way binding to editingScheme would break the moment the user
                    // types, and then selecting a different scheme would leave the stale typed name — so
                    // Save would write the newly selected scheme's colors under the wrong name. Re-seed
                    // imperatively, but only when the edited scheme's IDENTITY changes (editingSchemeChanged
                    // fires for select/new/save/delete, NOT for color tweaks), so a name being typed for a
                    // new or renamed scheme survives while its colors are edited.
                    text: root.controller ? root.controller.editingScheme : ""
                    selectByMouse: true
                    Connections {
                        target: root.controller
                        function onEditingSchemeChanged() {
                            schemeNameField.text = root.controller ? root.controller.editingScheme : ""
                        }
                    }
                }
                Button {
                    objectName: "saveSchemeButton"
                    text: qsTr("Save scheme")
                    highlighted: true
                    enabled: root.controller && !root.controller.locked && schemeNameField.text.trim().length > 0
                    onClicked: root.controller.saveColorScheme(schemeNameField.text.trim())
                }
                Item { Layout.fillWidth: true }
                // Delete lives on the color-scheme list row (hover trashcan / right-click), routed
                // through the settings page's confirmation dialog.
            }
        }
    }
}
