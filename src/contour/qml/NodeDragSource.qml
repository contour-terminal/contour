// vim:syntax=qml
// The carrier of a pane's or a tab's drag, and the one place the drag's protocol lives: the window's
// controller starts the drag (`begin`) and gets its end, and in between the OS runs it.
//
// The drag is AUTOMATIC: a real QDrag::exec(), which -- unlike the default Internal drag, confined to one
// window's scene -- crosses this process's top-level windows, so a DropArea of another window receives it.
// The payload rides in Drag.mimeData, under NodeDrag.mime, because drop.source is not reliable across an OS
// QDrag; a drop reads it back with getDataAsString(). What is dragged stays put; the OS cursor shows `image`.
import QtQuick
import Contour.Terminal

Item {
    id: source

    /// The window's controller; null while the window is torn down.
    property var controller: null
    /// Starts the drag at the controller (beginPaneDrag(), beginTabDrag()): returns the JSON the drag carries,
    /// empty for something that cannot be dragged, which starts nothing.
    property var begin: function() { return "" }
    /// The pixmap the OS drag cursor shows; empty for none.
    property url image: ""
    /// Where in `image` the pointer holds it.
    property point hotSpot: Qt.point(width / 2, height / 2)

    Drag.dragType: Drag.Automatic
    Drag.hotSpot.x: source.hotSpot.x
    Drag.hotSpot.y: source.hotSpot.y
    Drag.supportedActions: Qt.MoveAction
    Drag.proposedAction: Qt.MoveAction
    Drag.imageSource: source.image
    // The OS drag has returned with the drop action the window that took it ended it with: Qt.MoveAction from
    // any window that recognised the drop (a no-op and a refusal included), Qt.IgnoreAction for a drop outside
    // every window or a cancel -- which the controller tells apart, as Qt reports Escape like a drop outside.
    Drag.onDragFinished: (dropAction) => {
        if (source.controller)
            source.controller.finishNodeDrag(dropAction)
    }

    /// Picks the node up: the OS drag starts, and runs until it is dropped or cancelled.
    function carry() {
        if (source.Drag.active || !source.controller)
            return
        const payload = source.begin()
        if (payload === "")
            return
        const data = {}
        data[NodeDrag.mime] = payload
        source.Drag.mimeData = data
        // Blocks in the OS drag loop; dragFinished comes from inside it.
        source.Drag.active = true
    }
}
