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
import Contour.Terminal

// FocusScope, NOT a plain Item: main.qml's restoreTerminalFocus() (fired on every tab create/switch)
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
    // top-level instance in main.qml still sets `node:` declaratively at construction.
    property var node: null

    // Smallest first-child share a divider drag may persist; keeps both panes visible/grabbable.
    readonly property real minPaneRatio: 0.05

    // Clamp a raw drag ratio into [minPaneRatio, 1 - minPaneRatio]. Hoisted to the top-level item (and
    // kept a pure function) so the clamp arithmetic is unit-testable without synthesizing a drag.
    function clampRatio(raw) {
        return Math.max(minPaneRatio, Math.min(1.0 - minPaneRatio, raw))
    }

    // Leaf branch: a single terminal pane.
    Loader {
        anchors.fill: parent
        active: root.node !== null && root.node.isLeaf
        sourceComponent: TerminalPane {
            session: root.node ? root.node.session : null
            paneActive: root.node ? root.node.active : false
            onActivated: { if (root.node) root.node.activate() }
        }
    }

    // Split branch: two child PaneNodes in a SplitView.
    Loader {
        anchors.fill: parent
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

            // Explicit handle so the thickness has ONE source (vtworkspace::DefaultSplitHandleThickness,
            // surfaced as terminalSessions.splitHandleThickness) shared with the window-size solver.
            // Visuals reproduce the Qt Quick "Basic" style default delegate this replaces.
            handle: Rectangle {
                implicitWidth: splitView.orientation === Qt.Horizontal
                    ? terminalSessions.splitHandleThickness : splitView.width
                implicitHeight: splitView.orientation === Qt.Horizontal
                    ? splitView.height : terminalSessions.splitHandleThickness
                color: SplitHandle.pressed ? splitView.palette.mid
                    : (SplitHandle.hovered ? splitView.palette.midlight : splitView.palette.button)
            }

            // First child (URL-loaded to allow recursion).
            Loader {
                id: firstChild
                source: "PaneNode.qml"
                onLoaded: item.node = Qt.binding(function() { return root.node ? root.node.first : null })
                SplitView.preferredWidth: SplitView.view.orientation === Qt.Horizontal
                    ? SplitView.view.width * (root.node ? root.node.ratio : 0.5) : -1
                SplitView.preferredHeight: SplitView.view.orientation === Qt.Vertical
                    ? SplitView.view.height * (root.node ? root.node.ratio : 0.5) : -1
            }

            // Second child (fills the remaining space).
            Loader {
                id: secondChild
                source: "PaneNode.qml"
                onLoaded: item.node = Qt.binding(function() { return root.node ? root.node.second : null })
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
