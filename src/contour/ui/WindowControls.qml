// vim:syntax=qml
// Minimize / maximize-restore / close buttons for the frameless (client-side-decorated) window.
//
// Hidden on platforms that keep native window controls (macOS), where `useCustomWindowControls`
// is false.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

RowLayout {
    id: root

    required property var window

    spacing: 0

    component ControlButton: ToolButton {
        Layout.fillHeight: true
        implicitWidth: 44
        font.family: "monospace"
        font.pointSize: 10
        // Window controls must not steal keyboard focus from the terminal.
        focusPolicy: Qt.NoFocus
    }

    ControlButton {
        text: "—" // minimize
        onClicked: root.window.showMinimized()
    }

    ControlButton {
        text: root.window.visibility === Window.Maximized ? "❐" : "▢" // restore / maximize
        onClicked: {
            if (root.window.visibility === Window.Maximized)
                root.window.showNormal()
            else
                root.window.showMaximized()
        }
    }

    ControlButton {
        text: "✕" // close
        onClicked: root.window.close()

        // Red hover affordance for the close button.
        background: Rectangle {
            color: parent.hovered ? "#c42b1c" : "transparent"
        }
    }
}
