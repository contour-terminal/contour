// vim:syntax=qml
// Popup for the ContourTui style: an opaque, square, hairline-bordered box.
//
// Opaque is not a preference here. The application window is transparent so the terminal can show
// through it, and a popup that inherited any of that transparency would render its text over whatever
// happens to be scrolling underneath. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.Popup {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    padding: chromeStyle.borderWidth

    background: Rectangle {
        radius: chromeStyle.radius
        color: control.palette.window
        border.width: chromeStyle.borderWidth
        border.color: control.palette.mid
    }

    // The scrim behind a modal popup -- the command palette, the save-layout prompt, the tab color
    // flyout. QQuickOverlay creates no background item when these attached properties are unset, so a
    // style that omits them leaves a modal popup floating over an undimmed window that nonetheless
    // swallows every click: the user cannot tell what is still live, and gets no feedback from
    // clicking anything but the popup.
    T.Overlay.modal: Rectangle {
        color: chromeStyle.modalScrim(control.palette.shadow)
    }

    T.Overlay.modeless: Rectangle {
        color: chromeStyle.modelessScrim(control.palette.shadow)
    }
}
