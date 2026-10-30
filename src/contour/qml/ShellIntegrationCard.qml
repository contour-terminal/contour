// vim:syntax=qml
// The kit's shell integration card: the one-time offer to turn on command blocks, in the pane's bottom
// chrome. Its title and body, its two buttons and the failure under the line all come from the session
// (TerminalSession's shellIntegration* properties, window::CardStageRows' words), so this file decides
// only how a stage looks, never what it says.
//
// Offer asks "Turn on command blocks" with Install and Not Now. Install shows the exact line first
// (Confirm): the line is set in the session's terminal font in a read-only field the pointer can select,
// so it can be copied, and Add Line writes it. A failure keeps the card on Confirm with the reason under
// the line.
//
// PaneCard.qml places it in a band of its own below the cells, never over them: the terminal's bottom
// meets the band's top. On the native rows it is a rounded popover surface, `size.noticeCard` wide and
// centred; on the Terminal row it is a box of three cell rows (title, body, buttons) as wide as the pane
// less two cells, its buttons bracketed by the chip glyphs. Escape on the card is Not Now / Cancel.
import QtQuick
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: card
    objectName: "shellIntegrationCard"

    /// The TerminalSession whose card this is; null while a split rebinds.
    required property var session

    /// Whether the buttons are runs of bracketed cells (the Terminal row) rather than rounded shapes.
    readonly property bool bracketed: chromeStyle.chipOpenGlyph !== ""
    /// The card's stage, a window::CardStage's value; Hidden (zero) while there is no offer.
    readonly property int stage: session ? session.shellIntegrationCard : 0
    /// Whether the stage shows the exact line (Confirm) rather than the offer's sentence.
    readonly property bool confirming: stage === 2
    /// Whether the session has ended: its footer owns the pane's bottom then, and the card hides.
    readonly property bool ended: session ? session.sessionEnd !== 0 : false
    /// The card's surface: the popover's, which every ink here is read against.
    readonly property color fill: chromeStyle.paletteRevision < 0 ? "transparent"
                                                                  : chromeStyle.colors.paletteSurface
    /// The line between the card's rows: none on the Terminal row, whose rows touch.
    readonly property real rowGap: card.bracketed ? 0 : chromeStyle.space.s4

    /// Whether the card shows: an offer is made and the pane still runs. (`visible` is the effective
    /// visibility, which its host's band takes from this.)
    readonly property bool shown: card.stage !== 0 && !card.ended

    visible: card.shown
    // The band it stands in, which it never outgrows.
    readonly property real available: parent ? parent.width : Infinity
    width: Math.min(chromeStyle.size.noticeCard > 0 ? chromeStyle.size.noticeCard
                                                    : card.available - chromeStyle.cellWidth - chromeStyle.cellWidth,
                    card.available)
    height: column.implicitHeight + chromeStyle.space.noticeCardPadY + chromeStyle.space.noticeCardPadY

    Accessible.role: Accessible.AlertMessage
    Accessible.name: card.session ? card.session.shellIntegrationTitle : ""

    // Not Now on the offer, Cancel on the line.
    Keys.onEscapePressed: if (card.session) card.session.declineShellIntegrationCard()

    ElevationShadow {
        anchors.fill: parent
        z: -1
        level: chromeStyle.elevation.inPane
        radius: chromeStyle.radii.noticeCard
    }

    Rectangle {
        objectName: "shellIntegrationFill"
        anchors.fill: parent
        radius: chromeStyle.radii.noticeCard
        color: card.fill
    }

    ColumnLayout {
        id: column
        x: chromeStyle.space.noticeCardPadX
        y: chromeStyle.space.noticeCardPadY
        width: card.width - chromeStyle.space.noticeCardPadX - chromeStyle.space.noticeCardPadX
        spacing: card.rowGap

        Text {
            objectName: "shellIntegrationTitle"
            Layout.fillWidth: true
            Layout.preferredHeight: card.bracketed ? chromeStyle.cellHeight : implicitHeight
            text: card.session ? card.session.shellIntegrationTitle : ""
            textFormat: Text.PlainText
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            font.family: chromeStyle.type.body.family
            font.pixelSize: chromeStyle.type.body.pixelSize
            font.weight: Font.DemiBold
            color: chromeStyle.colors.paletteText
            Accessible.ignored: true
        }

        // The offer's sentence, or the line to add. Read-only and selectable, so the line can be copied; the
        // line is code, set in the terminal's own font, and is never wrapped through markup.
        TextEdit {
            id: body
            objectName: "shellIntegrationBody"
            Layout.fillWidth: true
            Layout.preferredHeight: card.bracketed ? chromeStyle.cellHeight : implicitHeight
            text: card.session ? card.session.shellIntegrationBody : ""
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: card.bracketed ? TextEdit.NoWrap : TextEdit.Wrap
            clip: card.bracketed
            font.family: card.confirming && card.session ? card.session.fontFamily : chromeStyle.type.tab.family
            font.pixelSize: chromeStyle.type.tab.pixelSize
            font.weight: chromeStyle.type.tab.weight
            color: chromeStyle.colors.paletteSecondaryText
            selectedTextColor: chromeStyle.colors.paletteSelectionText
            selectionColor: chromeStyle.colors.paletteSelectionFill
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        // Why the last install failed, under the line.
        Text {
            objectName: "shellIntegrationError"
            Layout.fillWidth: true
            visible: text !== ""
            text: card.session ? card.session.shellIntegrationError : ""
            textFormat: Text.PlainText
            wrapMode: card.bracketed ? Text.NoWrap : Text.Wrap
            elide: card.bracketed ? Text.ElideRight : Text.ElideNone
            font: chromeStyle.type.tab
            // The danger ink of the row's chrome: the status hue on the native rows, and on the Terminal row the
            // scheme's hue where it reads on the bar, else the bar's own text.
            color: chromeStyle.paletteRevision < 0 ? "transparent"
                 : chromeStyle.statusInk(chromeStyle.statusDanger)
            Accessible.ignored: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: chromeStyle.space.s4

            Item {
                Layout.fillWidth: true
            }

            CardButton {
                objectName: "shellIntegrationSecondary"
                label: card.session ? card.session.shellIntegrationSecondary : ""
                // The footer's wash on the card, as the kit's fill.3.
                fill: chromeStyle.paletteRevision < 0 ? "transparent"
                    : chromeStyle.colorOn(chromeStyle.roles.paneFooterFill, card.fill)
                ink: chromeStyle.colors.paletteText
                onClicked: if (card.session) card.session.declineShellIntegrationCard()
            }

            CardButton {
                objectName: "shellIntegrationPrimary"
                label: card.session ? card.session.shellIntegrationPrimary : ""
                fill: chromeStyle.accentColor
                ink: chromeStyle.paletteRevision < 0 ? "transparent"
                     : chromeStyle.onStatusColor(chromeStyle.statusRunning)
                onClicked: if (card.session) card.session.acceptShellIntegrationCard()
            }
        }
    }

    // A card button: its label in the tab type on a control-rounded fill, `size.footerButton` high; bracketed
    // by the chip glyphs on the Terminal row, where it has no fill of its own. Hovered or pressed, it lays
    // the wash away from its label over the fill at the style's hover strength. It takes no Tab focus:
    // assistive technology presses it through its action, and Escape answers the card from wherever the
    // focus is.
    component CardButton: Rectangle {
        id: button

        /// What the button says.
        property string label
        /// The label's colour.
        property color ink
        /// The button's fill.
        property color fill
        /// What the button lays over its fill while hovered or pressed: the wash away from the label, so
        /// the label's contrast only grows. Native rows only; the Terminal row's brackets are its feedback.
        readonly property color hoverWash: chromeStyle.paletteRevision < 0 ? "transparent"
                                                                          : chromeStyle.contrastWash(button.ink)

        /// A left click, or the accessibility press action.
        signal clicked()

        Layout.preferredWidth: buttonLabel.implicitWidth + (card.bracketed ? 0 : chromeStyle.space.s5 + chromeStyle.space.s5)
        Layout.preferredHeight: chromeStyle.size.footerButton
        radius: chromeStyle.radii.control
        color: button.fill

        Accessible.role: Accessible.Button
        Accessible.name: button.label
        Accessible.onPressAction: button.clicked()

        HoverHandler {
            id: buttonHover
        }

        Rectangle {
            anchors.fill: parent
            radius: button.radius
            // Native rows only: the Terminal row's brackets are all the feedback a cell has.
            color: (buttonHover.hovered || buttonTap.pressed) && !card.bracketed ? button.hoverWash
                                                                                 : "transparent"
        }

        Text {
            id: buttonLabel
            objectName: button.objectName + "Label"
            anchors.centerIn: parent
            text: card.bracketed ? chromeStyle.chipOpenGlyph + button.label + chromeStyle.chipCloseGlyph
                                 : button.label
            font: chromeStyle.type.tab
            color: button.ink
            Accessible.ignored: true
        }

        TapHandler {
            id: buttonTap
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: button.clicked()
        }
    }
}
