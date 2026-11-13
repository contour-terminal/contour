// vim:syntax=qml
// A picker's group header: the group's title above its first row, at the footnote size in semibold. Drawn by
// the first row of each group (the model names a group on that row only), so it takes no room elsewhere.
import QtQuick
import QtQuick.Controls

Item {
    id: root

    /// The group's title; empty on every row but a group's first, which hides the header.
    property string title: ""
    /// Where the title starts and how far it stays from the trailing edge: the picker's text inset.
    property real textInset: 0
    /// The prefix of the header's and its label's objectName (`<prefix>GroupHeader`, `<prefix>GroupHeaderLabel`).
    property string namePrefix: "picker"

    objectName: root.namePrefix + "GroupHeader"
    height: visible ? chromeStyle.size.paletteGroupHeader : 0
    visible: root.title !== ""

    Accessible.role: Accessible.Heading
    Accessible.name: root.title

    Label {
        objectName: root.namePrefix + "GroupHeaderLabel"
        x: root.textInset
        width: parent.width - x - root.textInset
        anchors.verticalCenter: parent.verticalCenter
        text: root.title
        // The footnote type at Font.DemiBold, built in C++: a copy of the token with its weight set here would
        // keep the token's weight.
        font: chromeStyle.type.footnoteSemibold
        elide: Text.ElideRight
        color: chromeStyle.colors.paletteSecondaryText
        Accessible.ignored: true
    }
}
