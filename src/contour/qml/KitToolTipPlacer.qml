// vim:syntax=qml
// Where a tooltip goes: below its control with the pointer inside its leading edge, above only where below
// has no room. Shared by KitToolTip and the styles' ToolTip, so a stray attached tooltip is placed alike. The
// popup's own `margins` (the shadow's reach) keep it inside the window sideways.
import QtQuick

QtObject {
    id: root

    /// The tooltip placed: a Popup whose parent is the control.
    required property var popup
    /// The pointer's x in the control's coordinates; the control's centre where nothing tracks the pointer.
    property real pointerX: popup.parent ? popup.parent.width / 2 : 0
    /// The gap between the control and the tooltip.
    readonly property real distance: chromeStyle.space.s2
    /// The pointer's offset from the tooltip's leading edge: the pointer lands inside the box.
    readonly property real pointerInset: chromeStyle.space.s3
    /// How often the tooltip has been placed: once per showing.
    property int placements: 0

    /// Puts the popup in place. Called once, as it begins to show.
    function place() {
        const control = root.popup.parent
        if (!control)
            return
        const window = control.Window.window
        root.popup.x = root.pointerX - root.pointerInset
        const bottom = control.mapToItem(null, 0, control.height).y + root.distance
        const fitsBelow = !window || bottom + root.popup.implicitHeight + root.popup.margins <= window.height
        root.popup.y = fitsBelow ? control.height + root.distance : -root.popup.implicitHeight - root.distance
        ++root.placements
    }
}
