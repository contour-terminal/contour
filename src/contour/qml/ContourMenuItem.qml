// vim:syntax=qml
// The row every in-scene menu is built from: the style's MenuItem, with its highlight's corner taken from the
// chrome row (`radii.menuRow`, the kit's `radius.menu · row`), and the row's chord, when it has one, in a
// trailing column.
//
// A Binding onto the style's background rather than a background of our own: Fusion, FluentWinUI3 and
// ContourTui each draw the highlight as a Rectangle in their own colours, and replacing it would trade
// their legible highlight/text pairing for one this file would have to keep in step with every style.
import QtQuick
import QtQuick.Controls

MenuItem {
    id: control

    /// The chord that runs the row too, drawn at the trailing edge in the secondary ink; empty for none.
    property string shortcutText: ""

    // The label makes room for the chord on the trailing side, which swaps with the reading direction. Only
    // a row with a chord is touched: one without keeps its style's own padding.
    Binding {
        target: control
        property: control.mirrored ? "leftPadding" : "rightPadding"
        value: control.horizontalPadding + shortcutLabel.implicitWidth + control.spacing
        when: control.shortcutText !== ""
        restoreMode: Binding.RestoreBindingOrValue
    }

    Text {
        id: shortcutLabel
        objectName: "menuItemShortcut"
        visible: control.shortcutText !== ""
        text: control.shortcutText
        x: control.mirrored ? control.horizontalPadding : control.width - width - control.horizontalPadding
        anchors.verticalCenter: parent.verticalCenter
        font: control.font
        color: control.highlighted ? control.palette.highlightedText
                                   : chromeStyle.colors.paletteSecondaryText
        opacity: control.enabled ? 1.0 : 0.5
        Accessible.ignored: true
    }

    Binding {
        target: control.background
        property: "radius"
        value: chromeStyle.radii.menuRow
        when: control.background !== null && control.background.radius !== undefined
    }
}
