// vim:syntax=qml
// A popup with a grid of predefined color swatches for trivially colorizing a tab (Windows-Terminal
// style). The palette comes from the authoritative vtmux model via the controller, so the GUI and
// any future network client offer the same choices. One click on a swatch sets the tab color.
import QtQuick
import QtQuick.Controls

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
            spacing: 4

            Repeater {
                // Null-guarded like TabStrip's currentIndex: the controller dies before this QML tree
                // on window close, and this binding re-evaluates against null during teardown.
                model: root.controller ? root.controller.tabColorPalette() : []
                delegate: Rectangle {
                    required property var modelData
                    width: 20
                    height: 20
                    radius: 3
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

        Button {
            width: grid.width
            text: qsTr("Reset Color")
            onClicked: {
                root.controller.resetTabColor(root.tabIndex)
                root.close()
            }
        }
    }
}
