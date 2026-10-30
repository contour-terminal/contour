// vim:syntax=qml
// Menu for the ContourLinux style: an opaque, rounded (10-pixel) surface lifted by the kit's e2, its
// rows inset by s1 so each row's rounded highlight sits inside the menu's corners.
//
// Opaque for the reason Popup.qml states. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T
import Contour.Ui

T.Menu {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    // The width of the widest entry. Computed here rather than left to the template: a Menu does NOT
    // derive contentWidth from its items when the style supplies its own contentItem, so without this
    // every entry longer than the background's minimum is elided. Reading `count` re-evaluates it as
    // items are added after construction (the new-tab profile list).
    contentWidth: {
        var widest = 0
        for (var i = 0; i < count; ++i) {
            var item = itemAt(i)
            if (item)
                widest = Math.max(widest, item.implicitWidth)
        }
        return Math.ceil(widest)
    }

    padding: chromeStyle.space.s1
    overlap: chromeStyle.space.s1

    // Not optional: Menu.addMenu() builds the row that represents a sub-menu by instantiating THIS
    // component, and a style that leaves it null makes insertItem() drop the sub-menu silently.
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
        // A floor for an empty or one-word menu: the narrowest a tab gets, which still reads as a menu.
        implicitWidth: chromeStyle.minTabWidth
        implicitHeight: chromeStyle.size.control
        radius: chromeStyle.radii.menu
        color: control.palette.window
        border.width: chromeStyle.borderWidth
        border.color: control.palette.mid

        ElevationShadow {
            anchors.fill: parent
            z: -1
            level: chromeStyle.elevation.e2
            radius: parent.radius
        }
    }

    // The dimming behind a modal or dimmed menu: QQuickOverlay creates no background item when these
    // are unset, so a modal menu would float over a fully lit window that still swallows every click.
    T.Overlay.modal: Rectangle {
        color: chromeStyle.modalScrim(control.palette.shadow)
    }

    T.Overlay.modeless: Rectangle {
        color: chromeStyle.modelessScrim(control.palette.shadow)
    }
}
