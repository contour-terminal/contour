// vim:syntax=qml
// The row every in-scene menu is built from: the style's MenuItem, with its highlight's corner taken from the
// chrome row (`radii.menuRow`, the kit's `radius.menu · row`).
//
// A Binding onto the style's background rather than a background of our own: Fusion, FluentWinUI3 and
// ContourTui each draw the highlight as a Rectangle in their own colours, and replacing it would trade
// their legible highlight/text pairing for one this file would have to keep in step with every style.
import QtQuick
import QtQuick.Controls

MenuItem {
    id: control

    Binding {
        target: control.background
        property: "radius"
        value: chromeStyle.radii.menuRow
        when: control.background !== null && control.background.radius !== undefined
    }
}
