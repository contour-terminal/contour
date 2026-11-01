// vim:syntax=qml
// The miniature of a pane card that follows the pointer while the pane is dragged: a card edged in the
// accent, its header tinted with it (the grip and the pane's name in the ink that reads on that tint), and
// the last lines the pane shows in its own font, lifted by a shadow. PaneHeader grabs it into the OS drag's
// image (Drag.imageSource); it is never shown in the scene itself.
//
// The grab takes the item with its shadow, so the item is the card plus the room the shadow reaches, and
// `hotSpot` is where the pointer holds the card: the middle of its header.
import QtQuick
import QtQuick.Window

Item {
    id: root
    objectName: "dragCard"

    /// The pane's name, as its header shows it.
    property string name
    /// The pane's own background: the card is its colour under the header's tint.
    property color background: "black"
    /// The colour the card is edged and its header tinted in: the chrome's accent.
    property color mark: "white"
    /// The colour of the body's lines.
    property color ink: "white"
    /// The last lines the pane shows, oldest first (TerminalSession.visibleTextTail).
    property var lines: []
    /// The family the lines are set in: the pane's own.
    property string lineFamily: "monospace"
    /// The card's width; its height follows from the header and the lines.
    property real cardWidth: 190
    /// The share of the scene the card lets through, so what lies beneath still reads.
    property real cardOpacity: 0.92
    /// How many lines the body has room for at least, so a pane with little on it still reads as a card.
    property int minimumLines: 2

    /// The header's fill: the pane's tone under the mark's tint.
    readonly property color headerFill: Qt.tint(root.background, Qt.alpha(root.mark, 0.45))
    /// The grip and the name, on the header's fill.
    readonly property color headerInk: chromeStyle.paletteRevision < 0 ? "transparent"
                                                                      : chromeStyle.onColor(root.headerFill)
    /// The body's text size and line height, in pixels.
    readonly property real lineSize: 10
    readonly property real lineHeight: Math.ceil(lineSize * 1.5)

    // The room the shadow reaches beyond the card on every side.
    readonly property real margin: Math.ceil(shadow.reach + Math.max(Math.abs(chromeStyle.elevation.e2.offsetX),
                                                                     Math.abs(chromeStyle.elevation.e2.offsetY)))
    readonly property real headerHeight: chromeStyle.paneHeaderHeight(Screen.devicePixelRatio)
    /// Where the pointer holds the card, in this item's coordinates.
    readonly property point hotSpot: Qt.point(margin + cardWidth / 2, margin + headerHeight / 2)

    width: cardWidth + margin + margin
    height: card.height + margin + margin

    Item {
        id: card
        x: root.margin
        y: root.margin
        width: root.cardWidth
        height: root.headerHeight + body.height
        opacity: root.cardOpacity

        ElevationShadow {
            id: shadow
            anchors.fill: parent
            level: chromeStyle.elevation.e2
            radius: chromeStyle.radii.paneCard
        }

        Rectangle {
            anchors.fill: parent
            radius: chromeStyle.radii.paneCard
            color: root.background
            border.width: chromeStyle.hairline(Screen.devicePixelRatio)
            border.color: root.mark
        }

        // The header: the pane's tone with the mark's tint, its top corners the card's.
        Item {
            width: parent.width
            height: root.headerHeight
            clip: true

            Rectangle {
                width: parent.width
                height: parent.height + chromeStyle.radii.paneCard
                radius: chromeStyle.radii.paneCard
                color: root.headerFill
            }

            // The grip: two bars, as the artboard draws it.
            Column {
                id: grip
                objectName: "dragCardGrip"
                x: chromeStyle.space.s4
                anchors.verticalCenter: parent.verticalCenter
                spacing: chromeStyle.space.s2

                Repeater {
                    model: 2
                    delegate: Rectangle {
                        width: chromeStyle.space.s6
                        height: chromeStyle.hairline(Screen.devicePixelRatio)
                        radius: height // clamped to half of it: a round-capped stroke
                        color: root.headerInk
                    }
                }
            }

            Text {
                objectName: "dragCardName"
                x: grip.x + grip.width + chromeStyle.space.s3
                width: parent.width - x - chromeStyle.space.s4
                anchors.verticalCenter: parent.verticalCenter
                text: root.name
                elide: Text.ElideRight
                font: chromeStyle.type.paneHeader
                color: root.headerInk
            }
        }

        Column {
            id: body
            y: root.headerHeight
            x: chromeStyle.space.s4
            width: parent.width - x - x
            topPadding: chromeStyle.space.s3
            bottomPadding: chromeStyle.space.s3
            spacing: 0

            Repeater {
                model: root.lines
                delegate: Text {
                    required property string modelData
                    width: body.width
                    text: modelData
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.family: root.lineFamily
                    font.pixelSize: root.lineSize
                    lineHeight: root.lineHeight
                    lineHeightMode: Text.FixedHeight
                    color: root.ink
                }
            }

            // Room for the lines a quiet pane does not have.
            Item {
                width: body.width
                height: Math.max(0, root.minimumLines - root.lines.length) * root.lineHeight
            }
        }
    }
}
