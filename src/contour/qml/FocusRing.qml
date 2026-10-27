// vim:syntax=qml
// The ring that shows keyboard focus in the chrome, drawn around its parent.
//
// Native rows ring the focused item in the accent (`colors.focusRing`, `size.focusRing` wide); the Terminal
// row cannot draw an outset, so it fills the item's cells instead (`colors.focusFill`, ring width 0). Both
// roles are none where they do not apply, so this file never learns which row it is on.
//
// The ring shows only for keyboard focus: its host sets `shown` from its own `activeFocus` and the
// controller's chrome focus, so a click never draws it.
import QtQuick

Rectangle {
    id: root
    objectName: "focusRing"

    /// Whether the ring is drawn: the host has active focus and the controller says the chrome has it.
    property bool shown: false
    /// How far the ring reaches past the host's edges; 0 draws it inside them, for a host whose own edge is
    /// the edge of what clips it (a flat tab fills its strip).
    property real reach: chromeStyle.size.focusRing
    /// The host's corner radius; the ring's is that plus its reach.
    property real hostRadius: 0

    // A ring is not an item of its own: the host has already told assistive technology it has focus.
    Accessible.ignored: true

    visible: root.shown
    anchors.fill: parent
    anchors.margins: -root.reach
    radius: root.hostRadius + root.reach
    border.width: chromeStyle.size.focusRing
    border.color: chromeStyle.colors.focusRing
    color: chromeStyle.colors.focusFill
}
