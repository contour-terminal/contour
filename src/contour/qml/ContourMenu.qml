// vim:syntax=qml
// The menu every context menu in the application is built from.
//
// Three properties that have to travel together, in one place rather than copied into each menu:
//
//   - popupType: an IN-SCENE popup. Since Qt 6.8 a Controls Menu defaults to a native platform menu
//     where one exists, and a native menu cannot host the sub-menus ActionContextMenu creates at
//     runtime -- the trap that made the tab menu come up EMPTY on Windows.
//   - background: the shared popup surface, which is where the drop shadow is drawn. An in-scene
//     popup has no OS surface for a compositor to shadow, so it has to be painted.
//   - margins: a minimum distance to the window edge. The window CLIPS an in-scene popup, so a menu
//     opened near an edge would otherwise have its shadow cut away. Qt's default (-1) lets a menu
//     overflow the window instead.
//
// Copying those three into each menu is what left ActionContextMenu's runtime sub-menus with a
// background but no margin -- a shadow drawn where nobody could see it.
import QtQuick
import QtQuick.Controls

Menu {
    id: control

    popupType: Popup.Item
    background: PopupSurface {}
    margins: chromeStyle.shadowMargin

    // Every colour a row paints -- its text, a disabled row's, a sub-menu's arrow, the highlight -- and the
    // surface beneath them, from the application palette directly, as KitDialog's are. A style's default
    // is not enough: a popup's inherited palette is taken before it reaches its window, and keeps the
    // style's (light) default once the window's palette arrives, which on a dark surface reads at under
    // 2:1. The roles are the ones Fusion's and ContourTui's menu files read; Fusion paints row text in
    // `text`, ContourTui in `windowText`, so both are the window's text.
    //
    // Per colour group rather than through the top-level `palette.text`, which sets every group at once:
    // a disabled row would then paint in the enabled text colour, and stop looking disabled.
    readonly property SystemPalette _sys: SystemPalette {
        colorGroup: SystemPalette.Active
    }
    readonly property SystemPalette _sysDisabled: SystemPalette {
        colorGroup: SystemPalette.Disabled
    }
    palette.active.window: control._sys.window
    palette.active.windowText: control._sys.windowText
    palette.active.text: control._sys.windowText
    palette.active.highlight: control._sys.highlight
    palette.active.highlightedText: control._sys.highlightedText
    palette.active.mid: control._sys.mid
    palette.inactive.window: control._sys.window
    palette.inactive.windowText: control._sys.windowText
    palette.inactive.text: control._sys.windowText
    palette.inactive.highlight: control._sys.highlight
    palette.inactive.highlightedText: control._sys.highlightedText
    palette.inactive.mid: control._sys.mid
    palette.disabled.window: control._sysDisabled.window
    palette.disabled.windowText: control._sysDisabled.windowText
    palette.disabled.text: control._sysDisabled.windowText
    palette.disabled.mid: control._sysDisabled.mid

    // The width of the widest entry, snapped up to a whole unit.
    //
    // Not something the template does for us: a Menu does NOT derive contentWidth from its items,
    // so its width falls back to whatever its BACKGROUND asks for -- and the moment a menu supplies
    // its own background, as this one does to carry the drop shadow, that fallback is the background's
    // implicit size rather than the style's. Without this every entry is elided to nothing and the
    // menu opens as a vertical line. ContourTui/Menu.qml carries the identical computation, and for
    // the identical reason.
    //
    // Reading `count` is what makes this re-evaluate: menus built at runtime (the terminal's context
    // menu, the new-tab profile list) add their items after construction, and the binding has to
    // follow.
    contentWidth: {
        var widest = 0
        for (var i = 0; i < control.count; ++i) {
            var item = control.itemAt(i)
            if (item)
                widest = Math.max(widest, item.implicitWidth)
        }
        return chromeStyle.widthQuantum * Math.ceil(widest / chromeStyle.widthQuantum)
    }
}
