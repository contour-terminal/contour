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
        // Wider than a tab-strip control, and from the token table rather than a literal: these sit in
        // the same bar as the tab strip, so in a one-row chrome a pinned 44px would be the one part
        // that never joins the grid.
        implicitWidth: chromeStyle.windowControlWidth
        // The glyphs below are Unicode window-control symbols (—, ❐, ▢, ✕), not code, so they render
        // fine in the default UI font. The chrome font is used rather than a pinned point size, and
        // resolveChromeFont() is what decides whether that is the platform UI font or the terminal's
        // — so no "monospace"/"Monospace" family is ever pinned HERE, which on some platforms
        // (notably macOS) has no match and forces an expensive font-family-alias populate on startup.
        font: chromeStyle.windowControlFont
        // Window controls must not steal keyboard focus from the terminal.
        focusPolicy: Qt.NoFocus

        // Flat, transparent chrome so the control blends into the title bar instead of showing an
        // opaque style button panel; a subtle highlight wash gives hover feedback. The close button
        // overrides this with its red hover fill below.
        background: Rectangle {
            color: control.hovered ? chromeStyle.wash(systemPalette.highlight) : "transparent"
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
        Accessible.name: qsTr("Minimize")
        onClicked: root.controller.minimizeWindow()
    }

    ControlButton {
        text: root.window.visibility === Window.Maximized ? "❐" : "▢" // restore / maximize
        Accessible.name: root.window.visibility === Window.Maximized ? qsTr("Restore") : qsTr("Maximize")
        onClicked: root.controller.toggleMaximized()
    }

    ControlButton {
        text: "✕" // close
        Accessible.name: qsTr("Close window")
        onClicked: root.window.close()

        // The close glyph turns white on the red hover fill for contrast.
        glyphColor: hovered ? "white" : systemPalette.windowText

        // Red hover affordance for the close button.
        background: Rectangle {
            color: parent.hovered ? "#c42b1c" : "transparent"
        }
    }
}
