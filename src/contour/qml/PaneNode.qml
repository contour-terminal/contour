// vim:syntax=qml
// Recursive renderer for one node of a tab's split-pane tree.
//
// A leaf node renders a single terminal pane (a ContourTerminal bound to the leaf's session); a
// split node renders a SplitView containing two child panes laid out along the node's orientation.
// The structure follows the vtworkspace::Pane tree, surfaced to QML as PaneProxy objects.
//
// A QML component cannot instantiate itself by name (Qt rejects that as "instantiated
// recursively"), so the split children are loaded by URL via Loader.setSource("PaneNode.qml", ...).
// This defers resolution and breaks the compile-time cycle.
//
// The splitter ratio is read one-way from the model (preferredWidth/Height) and written back only
// when the user finishes dragging (onResizingChanged) to avoid a binding loop.
import QtQuick
import QtQuick.Controls
import QtQuick.Window
import Contour.Terminal

// FocusScope, NOT a plain Item: Main.qml's restoreTerminalFocus() (fired on every tab create/switch)
// force-focuses this root. A plain Item would itself become the window scope's focus item — REPLACING
// the active TerminalPane, and since Qt Quick key events do not bubble, every subsequent key was
// silently swallowed (typing and all keybindings dead after creating a tab). As a FocusScope,
// activating the root forwards focus down the remembered focus chain to the active pane: nested
// PaneNodes are scopes too, so the active TerminalPane's forceActiveFocus() records the chain at
// every level, and the root re-activates exactly that leaf.
FocusScope {
    id: root

    // The PaneProxy for this node (leaf or split).
    //
    // NOT `required`: the recursive split children are URL-loaded (Loader { source: "PaneNode.qml" }) and can
    // only receive their node via the deferred `onLoaded: item.node = Qt.binding(...)` assignment below — which
    // runs AFTER the object is constructed. A `required property` must be satisfied at construction, so the
    // URL-Loader path could never satisfy it: Qt emitted "Required property node was not initialized" and left
    // each child with a null node, so neither inner Loader activated, no TerminalPane was created, and the
    // split pane rendered as the fully transparent window (the desktop showing through). Defaulting to null
    // makes the deferred assignment valid and matches the `root.node !== null` guards used throughout. The
    // top-level instance in Main.qml still sets `node:` declaratively at construction.
    property var node: null

    // True when this node lives inside a split. Only then does the active leaf draw its focus frame;
    // the root node of a tab starts false, and split children are told true by their parent's loaders.
    property bool inSplit: false

    // Smallest first-child share a divider drag may persist; keeps both panes visible/grabbable.
    readonly property real minPaneRatio: 0.05

    // Clamp a raw drag ratio into [minPaneRatio, 1 - minPaneRatio]. Hoisted to the top-level item (and
    // kept a pure function) so the clamp arithmetic is unit-testable without synthesizing a drag.
    function clampRatio(raw) {
        return Math.max(minPaneRatio, Math.min(1.0 - minPaneRatio, raw))
    }

    // Round a logical extent to whole device pixels at ratio `dpr` (#2040). The gap and the first card
    // both go through it, so the second card's origin (`first + gap`) lands on the device grid too. Pure
    // and hoisted for the same reason as clampRatio: the offscreen screen's ratio is always 1.
    function snapToDevice(logical, dpr) {
        return Math.round(logical * dpr) / dpr
    }

    // The session the backdrop takes its opacity from: the first leaf's. A split's panes share their
    // tab's profile, and an opaque backdrop would hide the desktop a translucent profile shows through.
    function firstLeafSession(node) {
        if (node === null)
            return null
        return node.isLeaf ? node.session : firstLeafSession(node.first)
    }

    // Only a split tree's root draws the gap behind its cards and insets them; a leaf root, and every
    // node inside a split, is edge to edge.
    readonly property bool isSplitRoot: !inSplit && node !== null && !node.isLeaf
    readonly property var backdropSession: firstLeafSession(node)

    // The gap's colour behind a split tree's cards, which the root's inset and every handle show.
    Rectangle {
        objectName: "splitBackdrop"
        anchors.fill: parent
        visible: root.isSplitRoot
        color: chromeStyle.colors.paneGapFill
        opacity: root.backdropSession ? root.backdropSession.opacity : 1.0
    }

    // Leaf branch: a single terminal pane.
    Loader {
        anchors.fill: parent
        active: root.node !== null && root.node.isLeaf
        sourceComponent: TerminalPane {
            session: root.node ? root.node.session : null
            paneActive: root.node ? root.node.active : false
            showFocusFrame: root.inSplit
            onActivated: { if (root.node) root.node.activate() }
        }
    }

    // Split branch: two child PaneNodes in a SplitView, inset from the content edge at the tree's root.
    Loader {
        anchors.fill: parent
        anchors.margins: root.isSplitRoot ? chromeStyle.space.paneInset : 0
        active: root.node !== null && !root.node.isLeaf
        sourceComponent: splitComponent
    }

    Component {
        id: splitComponent
        SplitView {
            id: splitView

            // While the SplitView is alive, root.node can momentarily evaluate to null during a
            // sibling collapse/rebind (rebuildActiveTabPaneProxies prunes proxies before the outer
            // Loader deactivates). Guard every root.node access here the same way the outer Loaders
            // do, so teardown does not raise "Cannot read property of null" TypeErrors.

            // orientation: vtworkspace::SplitState::Vertical (2) => side-by-side (Qt.Horizontal).
            orientation: (root.node && root.node.orientation === 2) ? Qt.Horizontal : Qt.Vertical

            // The device-pixel grid both the geometry and the clip path agree about (#2040). Shared by
            // the handle and the first child, because BOTH have to sit on it -- see below.
            //
            // KNOWN GAP: this is the SCREEN's ratio, while every C++ consumer of the boundary rounds at
            // QWindow::devicePixelRatio() -- TerminalDisplay::devicePixelRatio(), and the frame-derived
            // scale in TerminalRenderNode::prepare(). The two agree unless something overrides the ratio
            // per window (QT_SCALE_FACTOR, or a compositor handing Qt a window ratio distinct from its
            // screen's), and there they would differ. Closing it needs C++: QWindow::devicePixelRatio()
            // is a plain method, not a Q_PROPERTY, so QML cannot read it -- it would take a QML-facing
            // per-window object, which does not exist today (the only context properties are the
            // app-global terminalSessions and chromeStyle).
            readonly property real dprSnap: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1

            // The handle IS the gap between two cards: `space.paneGap` wide side by side, `space.paneGapRows`
            // high stacked -- the same tokens the window-size solver counts (window::splitChromeOf), so the
            // two cannot diverge. Its containment mask widens what grabs it to `space.splitHit` without
            // moving a card: Qt's documented way to make a SplitView handle easier to hit.
            //
            // Snapped to whole device pixels for the same reason the first child's extent is: the SECOND
            // pane's origin is `firstChild.width + gap`, so snapping only the extent leaves that origin
            // fractional whenever the gap is. At the very common 125% desktop scale a 6 point gap is 7.5
            // device pixels, and 175% makes it 10.5 -- users on exactly those scales would have seen the
            // #2040 artifact survive in the right/bottom pane.
            handle: Item {
                id: splitHandle
                objectName: "splitHandle"

                readonly property bool sideBySide: splitView.orientation === Qt.Horizontal
                readonly property real thickness: root.snapToDevice(
                    sideBySide ? chromeStyle.space.paneGap : chromeStyle.space.paneGapRows, splitView.dprSnap)
                // The hit area's own hover counts too: SplitHandle.hovered follows the SplitView's
                // hoverEnabled, which a platform without hover effects leaves off.
                readonly property bool engaged: SplitHandle.hovered || SplitHandle.pressed || hitHover.hovered
                // The Terminal row draws the split as a line of box glyphs; the native rows' gap is the line.
                readonly property string lineGlyph: chromeStyle.splitLineGlyphs[sideBySide ? 0 : 1]

                z: 1
                implicitWidth: sideBySide ? thickness : splitView.width
                implicitHeight: sideBySide ? splitView.height : thickness
                containmentMask: hitArea

                Rectangle {
                    anchors.fill: parent
                    visible: splitHandle.lineGlyph === ""
                    color: chromeStyle.density === "minimal"
                        ? chromeStyle.colors.paneSplitLine : chromeStyle.colors.paneGapFill
                    opacity: root.backdropSession ? root.backdropSession.opacity : 1.0
                }

                Item {
                    id: hitArea
                    objectName: "splitHitArea"
                    // Centred exactly, half a pixel and all: the hit area is a mask, not something drawn.
                    x: (parent.width - width) / 2
                    y: (parent.height - height) / 2
                    width: splitHandle.sideBySide
                        ? Math.max(chromeStyle.space.splitHit, splitHandle.thickness) : parent.width
                    height: splitHandle.sideBySide
                        ? parent.height : Math.max(chromeStyle.space.splitHit, splitHandle.thickness)

                    HoverHandler {
                        id: hitHover
                        cursorShape: splitHandle.sideBySide ? Qt.SplitHCursor : Qt.SplitVCursor
                    }
                }

                Rectangle {
                    objectName: "splitGrabber"
                    anchors.centerIn: parent
                    width: splitHandle.sideBySide ? chromeStyle.size.grabber : chromeStyle.size.grabberLength
                    height: splitHandle.sideBySide ? chromeStyle.size.grabberLength : chromeStyle.size.grabber
                    radius: chromeStyle.radii.grabber
                    color: chromeStyle.colors.paneGrabber
                    opacity: splitHandle.engaged ? 1.0 : 0.0
                    visible: chromeStyle.size.grabber > 0 && opacity > 0
                    Behavior on opacity { NumberAnimation { duration: chromeStyle.motion.fast } }
                }

                Repeater {
                    model: splitHandle.lineGlyph === "" ? 0
                        : Math.ceil(splitHandle.sideBySide ? splitHandle.height / chromeStyle.cellHeight
                                                           : splitHandle.width / chromeStyle.cellWidth)
                    delegate: Text {
                        required property int index
                        objectName: "splitLineGlyph"
                        x: splitHandle.sideBySide ? 0 : index * chromeStyle.cellWidth
                        y: splitHandle.sideBySide ? index * chromeStyle.cellHeight : 0
                        text: splitHandle.lineGlyph
                        font: chromeStyle.font
                        color: splitHandle.engaged ? splitView.palette.highlight : chromeStyle.colors.paneSplitLine
                    }
                }
            }

            // First child (URL-loaded to allow recursion).
            Loader {
                id: firstChild
                objectName: "splitFirst"
                source: "PaneNode.qml"
                onLoaded: {
                    item.inSplit = true
                    item.node = Qt.binding(function() { return root.node ? root.node.first : null })
                }
                // Snapped to whole DEVICE pixels, not left fractional (#2040).
                //
                // `view.width * ratio` is a real number, so the first child routinely gets a
                // fractional width and the second child a fractional origin. That origin reaches the
                // renderer twice, by two paths that round differently: the vertex transform composes
                // it unrounded (TerminalRenderNode::prepare -> composeItemToClip), while the scissor
                // rounds it to a whole pixel (qRound(originScene.x() * dpr)). Geometry and clip then
                // disagree by up to half a pixel, and because the glyph atlas is sampled with
                // QRhiSampler::Nearest the sample points land on texel boundaries -- a stem loses its
                // bright column for a scanline or two while its neighbours in the same row are
                // untouched.
                //
                // Snapping in LOGICAL coordinates would not be enough at a fractional device pixel
                // ratio, hence the round-trip through splitView.dprSnap: the pane boundary has to land
                // on a hardware pixel, which is the quantity both paths agree about. The handle is
                // snapped to the same grid, which is what makes the SECOND pane's origin
                // (`firstChild.width + gap`) whole as well.
                readonly property real dprSnap: splitView.dprSnap
                SplitView.preferredWidth: SplitView.view.orientation === Qt.Horizontal
                    ? root.snapToDevice(SplitView.view.width * (root.node ? root.node.ratio : 0.5), dprSnap) : -1
                SplitView.preferredHeight: SplitView.view.orientation === Qt.Vertical
                    ? root.snapToDevice(SplitView.view.height * (root.node ? root.node.ratio : 0.5), dprSnap) : -1
            }

            // Second child (fills the remaining space).
            Loader {
                id: secondChild
                objectName: "splitSecond"
                source: "PaneNode.qml"
                onLoaded: {
                    item.inSplit = true
                    item.node = Qt.binding(function() { return root.node ? root.node.second : null })
                }
                SplitView.fillWidth: true
                SplitView.fillHeight: true
            }

            // Write the ratio back to the model when the user stops dragging the splitter. Clamp
            // rather than discard: an earlier guard skipped writing the ratio whenever it fell outside
            // (0, 1), so dragging a divider to (or near) an edge was silently dropped and the divider
            // snapped back on the next model-driven rebuild. root.clampRatio() persists the user's drag
            // while never collapsing a pane to nothing.
            onResizingChanged: {
                if (!resizing && root.node && width > 0 && height > 0) {
                    var raw = (orientation === Qt.Horizontal)
                        ? firstChild.width / width
                        : firstChild.height / height
                    root.node.setRatio(root.clampRatio(raw))
                }
            }
        }
    }
}
