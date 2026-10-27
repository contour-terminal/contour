// vim:syntax=qml
// The color-scheme editor: a scrollable grid of swatch cards (default fg/bg, the 8 normal and 8
// bright ANSI colors, and the command-block slots) bound to SettingsController.schemeColors
// ({ key, label, color, colorSource }). Each card shows a live swatch and an editable "#rrggbb" field;
// committing applies the slot live through SettingsController.commitSchemeColor. A command-block slot the
// scheme leaves unset shows the colour the palette derives, says that it follows it, and is written only
// once a colour is committed to it; a slot pinned that way offers to follow again. A pinned footer bar
// offers a copy of the scheme to edit: only a GUI side-file scheme takes a change.
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
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                spacing: 0
                                Label {
                                    text: slot.modelData.label
                                    color: sys.windowText
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    objectName: "schemeColorFollows"
                                    visible: slot.modelData.colorSource === "derived"
                                    text: qsTr("Follows the scheme's other colors")
                                    color: root.subtleText
                                    font.pointSize: 8
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            ToolButton {
                                objectName: "followSchemeColorButton"
                                visible: slot.modelData.colorSource === "pinned"
                                enabled: root.controller && !root.controller.locked
                                text: "↺"
                                Accessible.name: qsTr("Follow the scheme's other colors again")
                                ToolTip.visible: hovered
                                ToolTip.text: Accessible.name
                                onClicked: if (root.controller) root.controller.resetSchemeColor(slot.modelData.key)
                            }
                            TextField {
                                id: hexField
                                objectName: "schemeColorField"
                                Layout.preferredWidth: 110
                                Layout.alignment: Qt.AlignVCenter
                                text: slot.modelData.color
                                enabled: root.controller && !root.controller.locked
                                selectByMouse: true
                                font.family: "monospace"
                                inputMask: "\\#HHHHHH"
                                Accessible.name: slot.modelData.label

                                /// Commits the colour the field shows, once all six digits are there.
                                function commit() {
                                    if (root.controller && hexField.acceptableInput)
                                        root.controller.commitSchemeColor(slot.modelData.key, hexField.text)
                                }
                                // Leaving the field commits a changed colour only, so tabbing through a derived
                                // swatch pins nothing. Return commits the colour shown, changed or not: that is
                                // how a derived slot keeps the colour it shows now.
                                onEditingFinished: if (hexField.text.toLowerCase() !== slot.modelData.color)
                                                       hexField.commit()
                                Keys.onReturnPressed: hexField.commit()
                                Keys.onEnterPressed: hexField.commit()
                            }
                        }
                    }
                }
            }
        }

        // The scheme's action bar, pinned below the scrolling palette.
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

                Button {
                    objectName: "duplicateSchemeButton"
                    text: qsTr("Duplicate to Edit")
                    enabled: root.controller && !root.controller.locked && root.controller.editingScheme.length > 0
                    onClicked: root.controller.duplicateScheme(root.controller.editingScheme)
                }
                Item { Layout.fillWidth: true }
                // Delete lives on the color-scheme list row (hover trashcan / right-click), routed
                // through the settings page's confirmation dialog.
            }
        }
    }
}
