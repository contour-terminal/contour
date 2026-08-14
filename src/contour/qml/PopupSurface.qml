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
// Why it is drawn with plain Rectangles and NOT with QtQuick.Effects' MultiEffect, which is the
// obvious tool for a soft shadow. MultiEffect has to be fed from `layer.enabled`, which puts the
// popup's whole background through an offscreen render target -- and this application does not own
// its compositing: the terminal paints OVER the QML scene graph in an afterRendering pass
// (TerminalDisplay), scissor-clipped to its item rect. A popup that renders through an FBO does not
// reliably survive that, and a context menu that opens but cannot be SEEN is far worse than a
// shadow with a little banding. Stacked translucent rectangles need no layer, no shader and no
// extra Qt module, and they compose exactly the way every other chrome item already does.
//
// Every shape and colour still comes from chromeStyle, so this file never learns which style is
// active.
import QtQuick

Rectangle {
    id: root

    // Opaque, for the reason ContourTui/Popup.qml states: the application window is transparent so
    // the terminal can show through it, and a popup that inherited any of that would render its text
    // over whatever happens to be scrolling underneath.
    color: root.palette.window
    radius: chromeStyle.radius
    border.width: chromeStyle.borderWidth
    border.color: root.palette.mid

    // The shadow's colour at full strength. Read once: each layer below takes an equal share of it.
    readonly property color _tint: chromeStyle.shadowTint(root.palette.shadow)

    // How many rectangles the falloff is built from. Enough that the banding stays under what the
    // eye picks out at this opacity, capped because each one is another rectangle to rasterize.
    readonly property int _layers: Math.min(10, Math.max(3, Math.round(chromeStyle.shadowBlur / 3)))

    // A child with negative z draws BEHIND its parent in Qt Quick, which is what puts these under
    // the opaque surface without a second item to hold them.
    //
    // Each is a filled rounded rectangle, larger than the last and all at the same low alpha, so
    // where many overlap -- close to the surface -- they sum to a strong shadow, and where only the
    // outermost reaches they sum to almost nothing. That accumulation IS the falloff.
    Repeater {
        model: (chromeStyle.hasPopupShadow && chromeStyle.shadowBlur > 0) ? root._layers : 0

        delegate: Rectangle {
            required property int index

            z: -1

            // Innermost layer is tight to the surface, outermost reaches the full blur radius.
            readonly property real spread: ((index + 1) / root._layers) * chromeStyle.shadowBlur

            anchors.fill: parent
            anchors.leftMargin: -spread
            anchors.rightMargin: -spread
            anchors.topMargin: -spread + chromeStyle.shadowOffsetY
            anchors.bottomMargin: -spread - chromeStyle.shadowOffsetY

            radius: root.radius + spread
            // An equal share each, so the overlap near the surface sums back to the style's opacity.
            color: Qt.rgba(root._tint.r, root._tint.g, root._tint.b, root._tint.a / root._layers)
        }
    }

    // The hard-edged case: no blur means no falloff to build, just the offset block a text-mode UI
    // casts. One filled rectangle at the style's full shadow opacity.
    Rectangle {
        visible: chromeStyle.hasPopupShadow && chromeStyle.shadowBlur <= 0
        z: -1
        anchors.fill: parent
        anchors.topMargin: chromeStyle.shadowOffsetY
        anchors.leftMargin: chromeStyle.shadowOffsetY
        anchors.bottomMargin: -chromeStyle.shadowOffsetY
        anchors.rightMargin: -chromeStyle.shadowOffsetY
        color: root._tint
        radius: root.radius
    }
}
