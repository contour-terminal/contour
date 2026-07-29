// vim:syntax=qml
// Menu for the ContourTui style: an opaque, square, hairline-bordered list of MenuItems.
//
// Opaque for the same reason Popup.qml is -- the window beneath is transparent. See ToolButton.qml for
// what a style file is.
import QtQuick
import QtQuick.Templates as T

T.Menu {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    // The width of the widest entry, snapped up to a whole cell. Computed here rather than left to
    // the template: a Menu does NOT derive contentWidth from its items when the style supplies its
    // own contentItem, so without this the popup keeps its background's minimum width and every
    // entry longer than that is elided -- and in a menu the entries are the whole content.
    //
    // Reading `count` is what makes this re-evaluate: menus built at runtime (the new-tab profile
    // list) add their items after construction, and the binding has to follow.
    contentWidth: {
        var widest = 0
        for (var i = 0; i < count; ++i) {
            var item = itemAt(i)
            if (item)
                widest = Math.max(widest, item.implicitWidth)
        }
        // Whole cells, so the popup's own edge lands on the grid like everything else in this style.
        return chromeStyle.widthQuantum * Math.ceil(widest / chromeStyle.widthQuantum)
    }

    // Just the border: a menu row supplies its own one-cell inset, and a second one here would leave
    // the highlight floating inside the frame instead of spanning it, which is not how a TUI menu
    // marks its cursor row.
    padding: chromeStyle.borderWidth
    overlap: chromeStyle.borderWidth

    // Not optional, and not merely the default look for a row: Menu.addMenu() builds the item that
    // represents a sub-menu by instantiating THIS component, so a style that leaves it null makes
    // insertItem() drop the sub-menu on the floor -- every "New Tab with Profile" and "Tab Bar" entry
    // silently missing, with the separators around it left behind.
    delegate: MenuItem {}

    contentItem: ListView {
        implicitHeight: contentHeight
        model: control.contentModel
        interactive: Window.window
                     ? contentHeight + control.topPadding + control.bottomPadding > control.height
                     : false
        clip: true
        currentIndex: control.currentIndex
    }

    background: Rectangle {
        implicitWidth: chromeStyle.cellWidth * 12
        implicitHeight: chromeStyle.controlHeight
        radius: chromeStyle.radius
        color: control.palette.window
        border.width: chromeStyle.borderWidth
        border.color: control.palette.mid
    }

    // The dimming behind a modal/dimmed menu. QQuickOverlay creates no background item at all when
    // these are unset, so without them a modal menu floats over a fully-lit window that still
    // swallows every click. @see Popup.qml, which carries the same pair for the same reason.
    T.Overlay.modal: Rectangle {
        color: chromeStyle.modalScrim(control.palette.shadow)
    }

    T.Overlay.modeless: Rectangle {
        color: chromeStyle.modelessScrim(control.palette.shadow)
    }
}
