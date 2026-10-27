// vim:syntax=qml
// The kit's font family picker: a button showing the family that opens a searchable list of `families`
// (a KitPopUp with `searchable`, so the list, its keyboard and its Escape are the pop-up's).
//
// `family` is the caller's; choosing one asks for it through the pop-up's `picked(value)`, whose value is the
// family. A family the list does not hold
// (one typed into contour.yml, or since uninstalled) is still shown, so the row never reads as another.
//
// Not editable, it is disabled: dimmed to `settings.disabledOpacity`, and it takes no input.
import QtQuick
import QtQuick.Controls

KitPopUp {
    id: root
    objectName: "kitFontPicker"

    /// The family in use.
    property string family
    /// The families on offer, as the controller lists them.
    property var families: []

    options: root.families
    value: root.family
    searchable: true
}
