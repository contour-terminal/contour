// vim:syntax=qml
// The kit's ExitedFooter: what an ended pane says at its bottom edge instead of a notice in the grid -- how
// the session ended ("Process exited with code 1."), a Restart when the end allows one, and Close.
//
// PaneCard.qml places it below the cells, never over them: the terminal's bottom meets the footer's top, so
// the ended program's last output stays readable above it. Return restarts the pane and the ClosePane
// binding closes it (TerminalSession's ended-pane keys), so the buttons are the pointer's way to the same
// two answers, and take no Tab focus; assistive technology presses them through their actions.
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
    readonly property string message: session ? session.endMessage : ""
    // The window's controller (Main.qml's `win`), which spells the configured chords.
    readonly property var controller: Window.window && Window.window.win ? Window.window.win : null
    readonly property color textColor: chromeStyle.paletteRevision < 0 ? "transparent"
        : chromeStyle.colorOn(chromeStyle.roles.paneFooterText, footer.background)

    visible: session ? session.sessionEnd !== 0 : false
    height: chromeStyle.size.paneFooter

    Accessible.role: Accessible.AlertMessage
    Accessible.name: footer.message

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
            color: chromeStyle.paletteRevision < 0 ? "transparent"
                 : chromeStyle.colorOn(chromeStyle.roles.paneFooterFill, footer.background)
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
            objectName: "exitedFooterRestart"
            visible: footer.session ? footer.session.restartOffer === 1 : false
            label: qsTr("Restart %1").arg(chromeStyle.restartGlyph)
            accessibleName: qsTr("Restart")
            fill: chromeStyle.accentColor
            ink: chromeStyle.paletteRevision < 0 ? "transparent"
                 : chromeStyle.onStatusColor(chromeStyle.statusRunning)
            onClicked: if (footer.session) footer.session.restart()
        }

        FooterButton {
            objectName: "exitedFooterClose"
            label: qsTr("Close %1").arg(footer.controller ? footer.controller.shortcutText("ClosePane") : "").trim()
            accessibleName: qsTr("Close")
            // The footer's own wash once more: a step above the footer, as the kit's fill.3 is.
            fill: chromeStyle.paletteRevision < 0 ? "transparent"
                  : chromeStyle.colorOn(chromeStyle.roles.paneFooterFill, footer.background)
            ink: footer.textColor
            onClicked: if (footer.session) footer.session.dismiss()
        }
    }

    // A footer button: its label in the tab type on a control-rounded fill, `size.footerButton` high.
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
        property alias fill: button.color

        /// A left click, or the accessibility press action.
        signal clicked()

        width: buttonLabel.implicitWidth + (footer.bracketed ? 0 : chromeStyle.space.s5 + chromeStyle.space.s5)
        height: chromeStyle.size.footerButton
        radius: chromeStyle.radii.control

        Accessible.role: Accessible.Button
        Accessible.name: button.accessibleName
        Accessible.onPressAction: button.clicked()

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
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: button.clicked()
        }
    }
}
