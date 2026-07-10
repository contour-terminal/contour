// vim:syntax=qml
// A popup with a grid of predefined color swatches for trivially colorizing a tab (Windows-Terminal
// style). The palette comes from the authoritative vtmux model via the controller, so the GUI and
// any future network client offer the same choices. One click on a swatch sets the tab color; a hex
// field below the swatches lets the user enter an arbitrary RGB color.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    required property var controller
    required property int tabIndex

    modal: true
    focus: true
    padding: 8

    // Live OS palette handle so the flyout frame adapts to dark/light in realtime (see TabContextMenu).
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Inherited by the "Reset Color" Button so its label stays readable in both modes.
    palette {
        button: systemPalette.button
        buttonText: systemPalette.buttonText
        window: systemPalette.window
        windowText: systemPalette.windowText
    }

    background: Rectangle {
        color: systemPalette.window
        border.color: systemPalette.mid
        border.width: 1
        radius: 4
    }

    contentItem: Column {
        spacing: 8

        Grid {
            id: grid
            columns: 8
            spacing: 8

            Repeater {
                // Null-guarded like TabStrip's currentIndex: the controller dies before this QML tree
                // on window close, and this binding re-evaluates against null during teardown.
                model: root.controller ? root.controller.tabColorPalette() : []
                delegate: Rectangle {
                    required property var modelData
                    objectName: "tabColorSwatch"
                    width: 30
                    height: 30
                    radius: 5
                    color: modelData
                    border.width: 1
                    border.color: Qt.rgba(0, 0, 0, 0.4)

                    HoverHandler { id: swatchHover }
                    scale: swatchHover.hovered ? 1.15 : 1.0
                    Behavior on scale { NumberAnimation { duration: 80 } }

                    TapHandler {
                        onTapped: {
                            root.controller.setTabColor(root.tabIndex, parent.modelData)
                            root.close()
                        }
                    }
                }
            }
        }

        // Arbitrary-color entry. Deliberately dependency-free (no QtQuick.Dialogs): a hard import of that
        // module is not installed in every runtime, and its absence would cascade up and break the whole
        // main.qml. Instead the user types a 6-digit hex RGB value; the preview updates live and Enter
        // (or clicking the preview) applies it. The "#rrggbb" string is coerced to a QColor by the same
        // controller.setTabColor() the preset swatches use, so the backend needs no new API.
        RowLayout {
            width: grid.width
            spacing: 8

            Label {
                text: "#"
                Layout.alignment: Qt.AlignVCenter
            }

            TextField {
                id: hexField
                objectName: "customColorField"
                Layout.fillWidth: true
                placeholderText: qsTr("RRGGBB")
                maximumLength: 6
                validator: RegularExpressionValidator { regularExpression: /[0-9A-Fa-f]{6}/ }
                onAccepted: root.applyCustomColor()
            }

            // Live preview of the typed color; click it to apply.
            Rectangle {
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                radius: 5
                border.width: 1
                border.color: Qt.rgba(0, 0, 0, 0.4)
                color: hexField.acceptableInput ? ("#" + hexField.text) : "transparent"
                TapHandler {
                    enabled: hexField.acceptableInput
                    onTapped: root.applyCustomColor()
                }
            }
        }

        Button {
            width: grid.width
            text: qsTr("Reset Color")
            onClicked: {
                root.controller.resetTabColor(root.tabIndex)
                root.close()
            }
        }
    }

    // Applies the typed hex color (when it is a complete, valid RGB value) through the same path as the
    // preset swatches, then closes. Null-guarded because the controller can be torn down during teardown.
    function applyCustomColor() {
        if (!hexField.acceptableInput || !root.controller)
            return;
        root.controller.setTabColor(root.tabIndex, "#" + hexField.text);
        root.close();
    }
}
