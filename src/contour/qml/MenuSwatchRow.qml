// vim:syntax=qml
// One row of colour swatches inside a menu: the "Tab Color" sub-menu's palette, each colour one click.
//
// A Control rather than a MenuItem. A MenuItem has a `triggered` of its own, which Enter fires as well as
// a click, and which closes the menu -- with nothing picked, since the order of the item's click and the
// swatch's tap is unspecified. This row triggers nothing itself: a swatch reports its pick through
// swatchPicked, and the menu that hosts the row (ActionContextMenu) decides when to close.
//
// It takes no keyboard focus: "Custom…" below it is the keyboard's way to a tab colour.
import QtQuick
import QtQuick.Controls

Control {
    id: root

    // The swatches, as the model's `swatches` row carries them:
    //   [ { title, color, actionId, enabled, checked } ]
    property var swatches: []

    // A swatch was clicked; carries that swatch's actionId.
    signal swatchPicked(int actionId)

    focusPolicy: Qt.NoFocus
    horizontalPadding: chromeStyle.labelPadding
    verticalPadding: chromeStyle.space.s2

    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: implicitContentHeight + topPadding + bottomPadding

    Accessible.role: Accessible.MenuItem
    Accessible.name: qsTr("Tab colours")

    contentItem: Row {
        spacing: chromeStyle.space.s2

        Repeater {
            model: root.swatches

            delegate: Rectangle {
                id: swatch

                required property var modelData

                objectName: "menuSwatch"
                width: chromeStyle.size.swatchWidth
                height: chromeStyle.size.swatchHeight
                radius: chromeStyle.radii.control
                // The colour is the model's: a swatch shows the very colour it applies.
                color: swatch.modelData.color
                // The checked swatch is ringed, in a colour of the style's (the scheme's foreground on the
                // Terminal row) rather than one derived from the swatch, so it reads on every swatch alike.
                border.width: swatch.modelData.checked ? chromeStyle.borderWidth : 0
                border.color: chromeStyle.colors.swatchRing

                Accessible.role: Accessible.Button
                Accessible.name: swatch.modelData.title

                TapHandler {
                    enabled: swatch.modelData.enabled
                    onTapped: root.swatchPicked(swatch.modelData.actionId)
                }
            }
        }
    }
}
