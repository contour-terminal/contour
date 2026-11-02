// vim:syntax=qml
// When a window's tooltips last showed, so the next one can tell whether it is "warm".
//
// A pointer that has just read one tooltip is still reading: the next shows with no delay. The state is a
// window's, held by its ThemedWindow (`tooltipClock`) and found by each KitToolTip through its control's
// window, so it is neither global nor shared between windows.
import QtQuick

QtObject {
    id: root

    /// How long after a tooltip was last shown the next shows with no delay, in milliseconds.
    property int warmMs: chromeStyle.motion.tooltipWarm
    /// The time source: milliseconds since the epoch. A test replaces it to drive time by hand.
    property var now: function() { return Date.now() }
    /// The time a tooltip was last shown or hidden, in `now` milliseconds; never when it is 0.
    property double lastActive: 0

    /// @return Whether a tooltip was shown, or hidden, within warmMs of now.
    function isWarm() {
        return root.lastActive > 0 && root.now() - root.lastActive < root.warmMs
    }

    /// Notes that a tooltip is showing, or has just hidden.
    function touch() {
        root.lastActive = root.now()
    }
}
