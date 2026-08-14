// vim:syntax=qml
// The surface every popup and menu in the application sits on: an opaque, style-shaped rectangle
// with a drop shadow.
//
// Why the shadow is DRAWN rather than asked of the compositor. Every menu here forces
// `popupType: Popup.Item` (see TabContextMenu.qml for why a native platform menu is not an option),
// which makes it an item inside the window's own scene rather than an OS window of its own -- and a
// thing that is not a surface cannot have a surface's shadow. That is the opposite of the main
// window, which publishes a real shadow to the compositor; see platform/WindowShadow.hpp.
//
// Why it is assigned per popup rather than added to the two style files we own. `ui_style` defaults
// to Native, which pins Fusion -- a Qt style, whose control files are not ours to edit. A shadow
// added only to styles/ContourTui/{Menu,Popup}.qml would therefore be invisible in the configuration
// almost everyone runs, and untestable besides: the GUI test binary pins one style for the whole
// process. Four popups here already replaced their background with a near-identical Rectangle for
// the same reason, so this also collapses those four copies into one.
//
// Every shape and colour still comes from chromeStyle, so this file never learns which style is
// active.
import QtQuick
import QtQuick.Effects

Rectangle {
    id: root

    // Opaque, for the reason ContourTui/Popup.qml states: the application window is transparent so
    // the terminal can show through it, and a popup that inherited any of that would render its text
    // over whatever happens to be scrolling underneath.
    color: root.palette.window
    radius: chromeStyle.radius
    border.width: chromeStyle.borderWidth
    border.color: root.palette.mid

    // A hard-edged shadow is just an offset rectangle, so it is drawn as one. Only a BLURRED shadow
    // goes through MultiEffect, which costs an offscreen render target and a shader pass per popup --
    // `layer.enabled` puts the whole surface through an FBO. The terminal chrome's shadow has no
    // blur at all, and would otherwise have paid for a shader that does nothing but translate.
    Rectangle {
        visible: chromeStyle.hasPopupShadow && chromeStyle.shadowBlur <= 0
        z: -1
        anchors.fill: parent
        anchors.topMargin: chromeStyle.shadowOffsetY
        anchors.leftMargin: chromeStyle.shadowOffsetY
        anchors.bottomMargin: -chromeStyle.shadowOffsetY
        anchors.rightMargin: -chromeStyle.shadowOffsetY
        color: chromeStyle.shadowTint(root.palette.shadow)
        radius: root.radius
    }

    layer.enabled: chromeStyle.hasPopupShadow && chromeStyle.shadowBlur > 0
    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowBlur: 1.0
        blurMax: chromeStyle.shadowBlur
        shadowVerticalOffset: chromeStyle.shadowOffsetY
        shadowColor: chromeStyle.shadowTint(root.palette.shadow)
        // The blur reaches beyond the rectangle that cast it; without this it would be cut off at
        // the source item's own bounds and the shadow would stop square at the popup's edge.
        autoPaddingEnabled: true
    }
}
