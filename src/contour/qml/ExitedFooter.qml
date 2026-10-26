// vim:syntax=qml
// The kit's ExitedFooter: what an ended pane says at its bottom edge instead of a notice in the grid -- how
// the session ended ("Process exited with code 1."), a Restart when the end allows one, and Close.
//
// PaneCard.qml places it below the cells, never over them: the terminal's bottom meets the footer's top, so
// the ended program's last output stays readable above it. Return restarts the pane and the ClosePane
// binding closes it (TerminalSession's ended-pane keys), so the buttons are the pointer's way to the same
// two answers, and take no Tab focus; assistive technology presses them through their actions, and hears
// the keys in the footer's description. A click elsewhere on the footer makes the pane the active one, as
// a click on its cells does, so that Return then reaches it.
//
// On the native rows it is a bar in the pane's own tone with a hairline above it; on the Terminal row it is
// one cell row of the bar, its buttons bracketed by the chip glyphs: `[Restart ↩] [Close Ctrl+Shift+X]`.
import QtQuick
import QtQuick.Window

Item {
    id: footer
    objectName: "exitedFooter"

    /// The TerminalSession of the ended pane; null while a split rebinds.
    required property var session
    /// The card's corner radius: the footer's bottom corners take it, its top edge stays square.
    property real cornerRadius: 0

    /// The pane's own background, which every colour here is resolved against.
    readonly property color background: session ? session.backgroundColor : "black"
    /// Whether the buttons are runs of bracketed cells (the Terminal row) rather than rounded shapes.
    readonly property bool bracketed: chromeStyle.chipOpenGlyph !== ""
    /// Whether the session has ended, so the footer shows.
    readonly property bool ended: session ? session.paneState === chromeStyle.paneStates.ended : false
    /// Whether the end offers Restart.
    readonly property bool restartOffered: session ? session.restartOffer === chromeStyle.paneEnds.restartOffered
                                                   : false
    readonly property string message: session ? session.endMessage : ""
    // The window's controller (Main.qml's `win`), which spells the configured chords.
    readonly property var controller: Window.window && Window.window.win ? Window.window.win : null
    // ClosePane's chord, asked for only while the footer shows: read when it appears, so a binding
    // reloaded meanwhile shows the next time a pane ends.
    readonly property string closeChord: ended && controller ? controller.shortcutText("ClosePane") : ""
    readonly property color textColor: chromeStyle.paletteRevision < 0 ? "transparent"
        : chromeStyle.colorOn(chromeStyle.roles.paneFooterText, footer.background)
    // The footer's wash over the pane's background; the Close button takes it once more.
    readonly property color washColor: chromeStyle.paletteRevision < 0 ? "transparent"
        : chromeStyle.colorOn(chromeStyle.roles.paneFooterFill, footer.background)

    // The message's floor: its first sentence, which a narrowing pane keeps before Close names its chord.
    readonly property string messageFloorText: {
        const end = footer.message.indexOf(". ")
        return end < 0 ? footer.message : footer.message.substring(0, end + 1)
    }

    // Whether Close can name its chord: the message's floor and the buttons, Close with its chord, fit
    // between the footer's leading and trailing margins. The order of sacrifice is the chord, then the
    // message; the buttons stay. A narrow pane's Close says only "Close" -- the chord stays in the
    // description.
    readonly property bool chordFits: {
        if (footer.closeChord === "")
            return false
        const floorWidth = messageFloor.advanceWidth > 0 ? Math.ceil(messageFloor.advanceWidth) + chromeStyle.space.s4
                                                         : 0
        const closeWidth = chordedClose.advanceWidth + (footer.bracketed ? 0 : 2 * chromeStyle.space.s5)
        const restartWidth = footer.restartOffered ? restartButton.width + chromeStyle.space.s4 : 0
        return floorWidth + closeWidth + restartWidth
               <= footer.width - chromeStyle.space.footerLeading - chromeStyle.space.s5
    }

    /// A click on the footer outside its buttons.
    signal activated()

    visible: footer.ended
    height: chromeStyle.size.paneFooter
    // Nothing reaches past the pane's own edge, however narrow the pane: not into the gap, not into the
    // neighbour.
    clip: true

    Accessible.role: Accessible.AlertMessage
    Accessible.name: footer.message
    Accessible.description: [footer.restartOffered ? qsTr("Return restarts it.") : "",
                             footer.closeChord !== "" ? qsTr("%1 closes it.").arg(footer.closeChord) : ""]
                            .filter(sentence => sentence !== "").join(" ")

    TapHandler {
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: footer.activated()
    }

    // The fill: the pane's background at the profile's opacity, as the terminal above it is, under the
    // footer's wash. Reaching one radius above the footer and clipped there, so the bottom corners follow
    // the card and the top edge meets the terminal square.
    Item {
        anchors.fill: parent
        clip: footer.cornerRadius > 0

        Rectangle {
            objectName: "exitedFooterBackground"
            anchors.fill: parent
            anchors.topMargin: -footer.cornerRadius
            radius: footer.cornerRadius
            color: footer.background
            opacity: footer.session ? footer.session.opacity : 1.0
        }

        Rectangle {
            objectName: "exitedFooterFill"
            anchors.fill: parent
            anchors.topMargin: -footer.cornerRadius
            radius: footer.cornerRadius
            color: footer.washColor
        }
    }

    // The hairline between the cells and the footer, in the card border's colour.
    Rectangle {
        objectName: "exitedFooterHairline"
        visible: !footer.bracketed
        width: parent.width
        height: chromeStyle.hairline(Screen.devicePixelRatio)
        color: chromeStyle.paletteRevision < 0 ? "transparent"
             : chromeStyle.colorOn(chromeStyle.roles.paneCardBorder, footer.background)
    }

    Text {
        objectName: "exitedFooterMessage"
        x: chromeStyle.space.footerLeading
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, buttons.x - chromeStyle.space.s4 - x)
        text: footer.message
        elide: Text.ElideRight
        font: chromeStyle.type.tab
        color: footer.textColor
        Accessible.ignored: true
    }

    Row {
        id: buttons
        x: footer.width - chromeStyle.space.s5 - width
        anchors.verticalCenter: parent.verticalCenter
        spacing: chromeStyle.space.s4

        FooterButton {
            id: restartButton
            objectName: "exitedFooterRestart"
            visible: footer.restartOffered
            label: qsTr("Restart %1").arg(chromeStyle.restartGlyph)
            accessibleName: qsTr("Restart")
            fill: chromeStyle.accentColor
            ink: chromeStyle.paletteRevision < 0 ? "transparent"
                 : chromeStyle.onStatusColor(chromeStyle.statusRunning)
            onClicked: if (footer.session) footer.session.restart()
        }

        FooterButton {
            objectName: "exitedFooterClose"
            label: footer.chordFits ? qsTr("Close %1").arg(footer.closeChord) : qsTr("Close")
            accessibleName: qsTr("Close")
            // The footer's own wash once more: a step above the footer, as the kit's fill.3 is.
            fill: footer.washColor
            // Pressed, the kit's strongest fill on the footer in place of the wash.
            pressedFill: chromeStyle.paletteRevision < 0 ? "transparent"
                       : chromeStyle.colorOn(chromeStyle.roles.paneHeaderActionHover, footer.background)
            ink: footer.textColor
            onClicked: if (footer.session) footer.session.dismiss()
        }
    }

    // Close as it reads with its chord, measured to decide whether it fits (chordFits).
    TextMetrics {
        id: chordedClose
        font: chromeStyle.type.tab
        text: footer.bracketed ? chromeStyle.chipOpenGlyph + qsTr("Close %1").arg(footer.closeChord) + chromeStyle.chipCloseGlyph
                               : qsTr("Close %1").arg(footer.closeChord)
    }

    // The message's floor as it reads, measured for chordFits.
    TextMetrics {
        id: messageFloor
        font: chromeStyle.type.tab
        text: footer.messageFloorText
    }

    // A footer button: its label in the tab type on a control-rounded fill, `size.footerButton` high. Under
    // the pointer it lays the footer's wash (fill.3) once more over that fill (native rows); pressed, it shows its
    // pressedFill instead, without the hover layer. Both resolve on the pane's own background, as the
    // header's actions do, and keep the label at 4.5:1 on every bundled scheme -- which a stronger fill
    // laid OVER Close's wash would not.
    component FooterButton: Rectangle {
        id: button

        /// What the button says.
        property string label
        /// What it shows: the label, bracketed on the Terminal row.
        readonly property string text: footer.bracketed ? chromeStyle.chipOpenGlyph + label + chromeStyle.chipCloseGlyph
                                                        : label
        /// What assistive technology announces it as.
        property string accessibleName
        /// The label's colour.
        property color ink
        /// The button's fill.
        property color fill
        /// The button's fill while pressed; its rest fill unless the host names a stronger one.
        property color pressedFill: fill

        /// A left click, or the accessibility press action.
        signal clicked()

        width: buttonLabel.implicitWidth + (footer.bracketed ? 0 : chromeStyle.space.s5 + chromeStyle.space.s5)
        height: chromeStyle.size.footerButton
        radius: chromeStyle.radii.control
        color: buttonTap.pressed ? button.pressedFill : button.fill

        Accessible.role: Accessible.Button
        Accessible.name: button.accessibleName
        Accessible.onPressAction: button.clicked()

        HoverHandler {
            id: buttonHover
        }

        Rectangle {
            objectName: button.objectName + "State"
            anchors.fill: parent
            radius: button.radius
            // Native rows only: the Terminal row's wash is an opaque bar colour, which would hide a
            // bracketed button's own fill (Restart's accent) and its ink with it.
            color: buttonHover.hovered && !buttonTap.pressed && !footer.bracketed ? footer.washColor : "transparent"
        }

        Text {
            id: buttonLabel
            objectName: button.objectName + "Label"
            anchors.centerIn: parent
            text: button.text
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
