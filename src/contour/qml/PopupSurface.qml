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

    // The shadow, as ONE nine-patch image carrying a real Gaussian.
    //
    // Stacked translucent rectangles were tried here first and are what this replaces: eight layers
    // across a twenty-four pixel blur are invisible against a dark terminal background and plainly
    // BANDED against a light one, and closing that gap by stacking more costs an item and a set of
    // bindings each while still only approaching what one blurred image gives exactly. The blur is
    // the same C++ one the window's own shadow is built from (platform/AlphaBlur.hpp), served
    // through an image provider because what QML needs here is a texture, not primitives.
    //
    // MultiEffect is the other obvious answer and is the one thing that must NOT be used: it has to
    // be fed from `layer.enabled`, which puts the popup through an offscreen render target, and this
    // application paints the terminal OVER the QML scene in an afterRendering pass -- a popup that
    // renders through an FBO does not reliably survive that, and a context menu that opens but
    // cannot be seen is far worse than any shadow.
    readonly property color _tint: chromeStyle.shadowTint(root.palette.shadow)

    // How far the shadow reaches beyond the surface, and the unstretched corner. Both are functions
    // of the same parameters the provider renders from, so the two cannot disagree.
    readonly property int _reach: chromeStyle.shadowBlur > 0
                                ? 3 * Math.max(1, Math.round((chromeStyle.shadowBlur + 1) / 2))
                                : 0
    readonly property int _margin: root._reach + Math.abs(chromeStyle.shadowOffsetY)
    readonly property int _corner: root._margin + chromeStyle.radius

    BorderImage {
        visible: chromeStyle.hasPopupShadow && root._margin > 0
        // A child with negative z draws BEHIND its parent in Qt Quick, which is what puts this under
        // the opaque surface without a second item to hold it.
        z: -1

        anchors.fill: parent
        anchors.margins: -root._margin

        source: "image://popupshadow/" + chromeStyle.shadowBlur
              + "/" + Math.round(chromeStyle.shadowOffsetY)
              + "/" + chromeStyle.radius
              // The colour WITHOUT its leading '#': that character starts a URL fragment, and Qt
              // would strip everything after it before the provider ever saw the id.
              + "/" + root._tint.toString().substring(1)

        // Corners drawn at their natural size, edges stretched between them -- the same nine-patch
        // arrangement the compositor assembles the window's shadow from, and for the same reason: a
        // popup is any size, and the falloff along an edge does not vary with its length.
        border { left: root._corner; top: root._corner; right: root._corner; bottom: root._corner }
        horizontalTileMode: BorderImage.Stretch
        verticalTileMode: BorderImage.Stretch
    }
}
