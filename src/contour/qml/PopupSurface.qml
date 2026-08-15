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

    // A background is what gives an unsized control its minimum: Qt Quick Controls compute a
    // popup's width as max(implicitBackgroundWidth, contentWidth + padding), and a plain Rectangle's
    // implicit size is ZERO. Replacing a style's background without restoring this is what collapsed
    // every menu to a two-pixel vertical line -- the width of its own border and nothing else.
    // The numbers mirror ContourTui/Menu.qml's background, which states the same contract.
    // cellWidth, NOT widthQuantum: the quantum is 1.0 in the default style, so twelve of them is
    // twelve PIXELS rather than the twelve character widths ContourTui/Menu.qml's background means.
    // Menus escape that through their own contentWidth; a popup that falls back to this would not.
    implicitWidth: chromeStyle.cellWidth * 12
    implicitHeight: chromeStyle.controlHeight

    // Opaque, for the reason ContourTui/Popup.qml states: the application window is transparent so
    // the terminal can show through it, and a popup that inherited any of that would render its text
    // over whatever happens to be scrolling underneath.
    color: root.palette.window
    radius: chromeStyle.radius
    border.width: chromeStyle.borderWidth
    border.color: root.palette.mid

    // The shadow's colour at full strength. Read once: each layer below takes an equal share of it.
    readonly property color _tint: chromeStyle.shadowTint(root.palette.shadow)

    // How many rectangles the falloff is built from. One for a hard-edged shadow -- the terminal
    // chrome's, which has no blur at all -- and otherwise enough that the banding stays under what
    // the eye picks out at this opacity.
    readonly property int _layers: chromeStyle.shadowBlur > 0
                                 ? Math.max(2, Math.round(chromeStyle.shadowBlur / 3))
                                 : 1

    // A hard-edged shadow is displaced sideways as well as down -- the offset block a text-mode UI
    // casts. A blurred one spreads evenly and is displaced only vertically.
    readonly property real _shiftX: chromeStyle.shadowBlur > 0 ? 0 : chromeStyle.shadowOffsetY

    // A child with negative z draws BEHIND its parent in Qt Quick, which is what puts these under
    // the opaque surface without a second item to hold them.
    //
    // Each is a filled rounded rectangle, larger than the last and all at the same low alpha, so
    // where many overlap -- close to the surface -- they sum to a strong shadow, and where only the
    // outermost reaches they sum to almost nothing. That accumulation IS the falloff, and the
    // hard-edged case is simply its one-layer, zero-spread degenerate form rather than a second
    // rectangle that has to keep agreeing about z-order, radius and tint.
    Repeater {
        model: chromeStyle.hasPopupShadow ? root._layers : 0

        delegate: Rectangle {
            required property int index

            z: -1

            // A rounded Rectangle turns antialiasing on implicitly, which emits the fringe geometry
            // and a smooth-material node for an edge drawn here at a few percent alpha -- where no
            // one can see it. Eight layers per popup paid that for nothing.
            antialiasing: false

            // Innermost layer is tight to the surface, outermost reaches the full blur radius.
            readonly property real spread: ((index + 1) / root._layers) * chromeStyle.shadowBlur

            anchors.fill: parent
            anchors.leftMargin: -spread + root._shiftX
            anchors.rightMargin: -spread - root._shiftX
            anchors.topMargin: -spread + chromeStyle.shadowOffsetY
            anchors.bottomMargin: -spread - chromeStyle.shadowOffsetY

            radius: root.radius + spread
            // An equal share each, so the overlap near the surface sums back to the style's opacity.
            color: Qt.rgba(root._tint.r,
                           root._tint.g,
                           root._tint.b,
                           root._tint.a / root._layers)
        }
    }
}
