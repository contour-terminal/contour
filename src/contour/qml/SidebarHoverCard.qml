// vim:syntax=qml
// The card a sidebar row shows while it is hovered: the row's whole reason -- monospace when it is a command
// the program asks to run -- what it is about (program · folder · time in its state), and the two things the
// user does about it: go to the pane, and count it as seen. Everything it says is the row's own roles; the
// surface is the popups' (PopupSurface), so it takes the row's shape and shadow.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: card
    objectName: "sidebarHoverCard"

    /// The sidebar's model, which the buttons act through.
    required property var sidebar
    /// The row the card is about (a SidebarRow), or null.
    property var row: null

    /// Whether the pointer is on the card, which keeps it open after it left the row.
    readonly property bool hovered: cardHover.hovered


    /// Shows the card beside @p row.
    function showFor(target) {
        card.row = target
        const at = target.mapToItem(card.parent, target.width, 0)
        card.x = at.x
        card.y = at.y
        card.open()
    }

    popupType: Popup.Item
    width: chromeStyle.sidebar.hoverCardWidth
    padding: chromeStyle.space.s4
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: PopupSurface {
        color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.paletteSurface
    }

    /// The pointer left the card: it closes, unless the pointer went back to the row it is about.
    signal left()

    contentItem: ColumnLayout {
        spacing: chromeStyle.space.s3

        HoverHandler {
            id: cardHover
            onHoveredChanged: if (!cardHover.hovered) card.left()
        }

        // The reason: its words in the body face, and its command, when it has one, in the monospace face
        // after them; a long command wraps within the card.
        Flow {
            Layout.fillWidth: true
            visible: card.row !== null && card.row.fullReason !== ""

            Text {
                objectName: "sidebarHoverCardReason"
                textFormat: Text.PlainText
                text: card.row !== null ? card.row.fullReasonLabel : ""
                font: chromeStyle.type.body
                color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.paletteText
            }

            Text {
                objectName: "sidebarHoverCardReasonCommand"
                visible: text !== ""
                width: Math.min(implicitWidth, parent.width)
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                text: card.row !== null ? card.row.fullReasonCommand : ""
                font: chromeStyle.type.monospace
                color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.paletteText
            }
        }

        Text {
            objectName: "sidebarHoverCardDetail"
            Layout.fillWidth: true
            visible: text !== ""
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            text: card.row !== null ? card.row.hoverDetail : ""
            font: chromeStyle.type.footnote
            color: chromeStyle.paletteRevision < 0 ? "transparent" : chromeStyle.colors.paletteSecondaryText
        }

        RowLayout {
            spacing: chromeStyle.space.s3

            Button {
                objectName: "sidebarHoverCardGoToPane"
                text: qsTr("Go to pane")
                onClicked: {
                    if (card.row !== null)
                        card.sidebar.activate(card.row.index)
                    card.close()
                }
            }

            Button {
                objectName: "sidebarHoverCardMarkSeen"
                text: qsTr("Mark Seen")
                onClicked: {
                    if (card.row !== null)
                        card.sidebar.markSeen(card.row.index)
                    card.close()
                }
            }
        }
    }
}
