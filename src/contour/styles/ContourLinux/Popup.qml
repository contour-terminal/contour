// vim:syntax=qml
// Popup for the ContourLinux style: an opaque, rounded (12-pixel) popover lifted by the kit's e3.
//
// Opaque is not a preference: the application window is transparent so the terminal can show through
// it, and a popup that inherited any of that would render its text over whatever scrolls underneath.
// See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T
import Contour.Ui

T.Popup {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    padding: chromeStyle.space.s4

    background: Rectangle {
        radius: chromeStyle.radii.popover
        color: control.palette.window
        border.width: chromeStyle.borderWidth
        border.color: control.palette.mid

        ElevationShadow {
            anchors.fill: parent
            z: -1
            level: chromeStyle.elevation.e3
            radius: parent.radius
        }
    }

    // The scrim behind a modal popup -- the command palette, the save-layout prompt, the tab colour
    // flyout. QQuickOverlay creates no background item when these attached properties are unset.
    T.Overlay.modal: Rectangle {
        color: chromeStyle.modalScrim(control.palette.shadow)
    }

    T.Overlay.modeless: Rectangle {
        color: chromeStyle.modelessScrim(control.palette.shadow)
    }
}
