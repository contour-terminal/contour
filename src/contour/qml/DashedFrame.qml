// vim:syntax=qml
// A dashed rectangular outline: the "<name>'s place" a dragged pane card leaves behind, and the slot a tab
// strip shows where a dropped node would land.
//
// Four rows of dashes, one along each edge, each as long as `dash` with `gap` between them, centred on the
// edge so both ends of an edge are cut alike. The corners are left open: a dashed outline reads the same
// there, and a QML Rectangle's border cannot be dashed (Qt Quick Shapes would, but is a module of its own
// to deploy for one outline).
import QtQuick

Item {
    id: root

    /// The dashes' colour.
    property color color: "white"
    /// The outline's thickness.
    property real thickness: 1.5
    /// How long a dash is, and the gap after it.
    property real dash: 6
    property real gap: 4
    /// How far the corners stay open, so the outline looks rounded where its host is.
    property real cornerInset: 0

    // How many dashes fit along `length` once the corner insets are taken.
    function count(length) {
        const usable = Math.max(0, length - 2 * root.cornerInset)
        return Math.max(0, Math.floor((usable + root.gap) / (root.dash + root.gap)))
    }

    // Where the first dash of `n` dashes starts along `length`, so the row is centred.
    function start(length, n) {
        return (length - (n * root.dash + Math.max(0, n - 1) * root.gap)) / 2
    }

    Repeater {
        model: root.count(root.width)
        delegate: Item {
            required property int index
            readonly property real along: root.start(root.width, root.count(root.width)) + index * (root.dash + root.gap)
            Rectangle { x: parent.along; y: 0; width: root.dash; height: root.thickness; color: root.color }
            Rectangle { x: parent.along; y: root.height - root.thickness; width: root.dash; height: root.thickness; color: root.color }
        }
    }

    Repeater {
        model: root.count(root.height)
        delegate: Item {
            required property int index
            readonly property real along: root.start(root.height, root.count(root.height)) + index * (root.dash + root.gap)
            Rectangle { x: 0; y: parent.along; width: root.thickness; height: root.dash; color: root.color }
            Rectangle { x: root.width - root.thickness; y: parent.along; width: root.thickness; height: root.dash; color: root.color }
        }
    }
}
