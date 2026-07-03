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
    // The WindowController — the ONLY component allowed to change window show modes. Calling
    // window.showMaximized()/showNormal() directly here would skip its size-increment protocol
    // (stale WM increments while maximized, no grid-snap hints after restore).
    required property var controller

    spacing: 0

    // Live OS palette so the glyphs adapt to dark/light themes (see TabContextMenu). The window is
    // transparent, so an explicit contentItem with a palette-driven color keeps every control glyph
    // legible in both light and dark modes regardless of the active style or hover state.
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

        // Flat, transparent chrome so the control blends into the title bar instead of showing an
        // opaque style button panel; a subtle highlight wash gives hover feedback. The close button
        // overrides this with its red hover fill below.
        background: Rectangle {
            color: control.hovered ? Qt.rgba(systemPalette.highlight.r,
                                             systemPalette.highlight.g,
                                             systemPalette.highlight.b,
                                             0.25)
                                   : "transparent"
        }

        // Explicit, theme-aware glyph rendering keeps the glyph legible on the transparent title bar
        // in both light and dark modes.
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
        onClicked: root.controller.minimizeWindow()
    }

    ControlButton {
        text: root.window.visibility === Window.Maximized ? "❐" : "▢" // restore / maximize
        onClicked: root.controller.toggleMaximized()
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
