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

    // Live OS palette so the glyphs adapt to dark/light themes (see TabContextMenu). The window is
    // transparent and no Qt Quick Controls style is configured, so the Basic ToolButton's default content
    // color does not resolve to a readable value here — most visibly, the close button (which overrides its
    // background) rendered an *invisible* glyph until hover. An explicit contentItem with a palette-driven
    // color makes every control glyph visible in both modes, regardless of hover.
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    component ControlButton: ToolButton {
        id: control
        Layout.fillHeight: true
        implicitWidth: 44
        font.family: "monospace"
        font.pointSize: 10
        // Window controls must not steal keyboard focus from the terminal.
        focusPolicy: Qt.NoFocus

        // Explicit, theme-aware glyph rendering — the Basic style's default text color is unreliable on
        // this transparent, style-less window.
        property color glyphColor: systemPalette.windowText
        contentItem: Text {
            text: control.text
            font: control.font
            color: control.glyphColor
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
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

        // The close glyph turns white on the red hover fill for contrast.
        glyphColor: hovered ? "white" : systemPalette.windowText

        // Red hover affordance for the close button.
        background: Rectangle {
            color: parent.hovered ? "#c42b1c" : "transparent"
        }
    }
}
