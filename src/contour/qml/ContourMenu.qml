// vim:syntax=qml
// The menu every context menu in the application is built from.
//
// Three properties that have to travel together, in one place rather than copied into each menu:
//
//   - popupType: an IN-SCENE popup. Since Qt 6.8 a Controls Menu defaults to a native platform menu
//     where one exists, and a native menu cannot host the sub-menus ActionContextMenu creates at
//     runtime -- the trap that made TabContextMenu come up EMPTY on Windows.
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
    popupType: Popup.Item
    background: PopupSurface {}
    margins: chromeStyle.shadowMargin
}
