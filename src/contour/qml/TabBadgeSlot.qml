// vim:syntax=qml
// A tab's badge slot: one contour::window::TabBadgeKind, drawn the way the style's look table says
// (UiStyleProvider::badgeLook). The Pill shape draws the look's mark -- a dot, a ring that turns into a
// dial once the program gives a progress, a disc carrying the glyph, or the glyph alone -- and the Flat
// shape draws the glyph as text, as a terminal cell would. A kind the table does not know draws nothing.
import QtQuick
import QtQuick.Window

Item {
    id: root

    Accessible.role: Accessible.StaticText
    Accessible.name: root.known ? root.look.name : ""

    // The badge, a contour::window::TabBadgeKind as its underlying value.
    required property int kind
    // Percent done for a Working badge; -1 when the program gave none, which spins the mark instead.
    property int progress: -1

    // How the kind is drawn -- { glyph, mark, ink, name } -- or empty for a kind the table does not know.
    // Reads paletteRevision so the ink re-evaluates on a light/dark flip or an accent change.
    readonly property var look: chromeStyle.paletteRevision < 0 ? ({}) : chromeStyle.badgeLook(root.kind)
    readonly property bool known: root.kind > 0 && root.look.glyph !== undefined
    // Which of the two drawings below is the slot's.
    readonly property bool pill: chromeStyle.layout.isPill
    readonly property color ink: root.known ? root.look.ink : "transparent"
    // The ring is Working's mark: with a progress it stands still as a dial of that many degrees.
    readonly property bool working: root.known && root.look.mark === "ring"
    readonly property bool spinning: root.working && root.progress < 0
    readonly property real progressAngle: root.working && root.progress >= 0 ? 360 * root.progress / 100.0 : 0
    // Whether the window holding the slot is on screen, and whether the chrome animates at all: a program
    // may work for hours in a minimized window, and under Reduce Motion nothing spins.
    readonly property bool windowShown: Window.visibility !== Window.Hidden
                                        && Window.visibility !== Window.Minimized

    visible: root.known
    implicitWidth: root.known ? root.edge : 0
    implicitHeight: root.edge
    readonly property real edge: root.pill ? chromeStyle.size.badge : chromeStyle.badgeHeight

    // Spins a working badge while the program gave no progress. Its own item, so that the marks' own
    // rotation stays a binding the animation never writes over.
    Item {
        id: spinner
        anchors.fill: parent

        RotationAnimation on rotation {
            objectName: "tabBadgeSpinner"   // findChild() handle for the GUI test
            running: root.spinning && root.windowShown && chromeStyle.motion.animates
            loops: Animation.Infinite
            from: 0
            to: 360
            duration: chromeStyle.motion.spinner
            // Back upright when it stops, as the progress bar restores its opacity.
            onRunningChanged: if (!running) spinner.rotation = 0
        }

        // The Pill shape's mark.
        Item {
            id: mark
            objectName: "tabBadgeMark"   // findChild() handle for the GUI test
            anchors.centerIn: parent
            visible: root.pill
            width: mark.mark === "dot" ? chromeStyle.size.badgeDot : chromeStyle.size.badge
            height: width
            // Which drawing is shown: "dot", "ring", "disc" or "glyph", as the look table names it.
            readonly property string mark: root.known ? root.look.mark : "none"
            // How far round the ring is drawn: the progress as an angle, a quarter while it spins.
            readonly property real sweep: root.spinning ? 90 : root.progressAngle
            readonly property real round: width / 2

            Rectangle {
                anchors.fill: parent
                visible: mark.mark === "dot" || mark.mark === "disc"
                radius: mark.round
                color: root.ink
            }

            Canvas {
                id: ring
                anchors.fill: parent
                visible: mark.mark === "ring"
                readonly property color ink: root.ink
                readonly property real sweep: mark.sweep
                readonly property real stroke: chromeStyle.size.progressBar
                onInkChanged: requestPaint()
                onSweepChanged: requestPaint()
                onWidthChanged: requestPaint()
                onVisibleChanged: requestPaint()
                onPaint: {
                    const context = getContext("2d");
                    context.reset();
                    const radius = ring.width / 2 - ring.stroke;
                    if (!ring.visible || radius <= 0)
                        return;
                    context.lineWidth = ring.stroke;
                    context.lineCap = "round";
                    context.strokeStyle = ring.ink;
                    const centre = ring.width / 2;
                    const start = -Math.PI / 2;
                    context.beginPath();
                    context.arc(centre, centre, radius, start, start + (ring.sweep * Math.PI / 180));
                    context.stroke();
                }
            }

            // The disc carries the glyph in the colour that reads on the ink; the glyph mark is the ink.
            Text {
                anchors.centerIn: parent
                visible: mark.mark === "disc" || mark.mark === "glyph"
                textFormat: Text.PlainText
                text: root.known ? root.look.glyph : ""
                font: chromeStyle.badgeFont
                color: mark.mark === "disc" ? chromeStyle.onColor(root.ink) : root.ink
            }
        }

        // The Flat shape's glyph: one cell of text, standing at the progress's angle when there is one.
        Text {
            id: glyph
            objectName: "tabBadgeGlyph"   // findChild() handle for the GUI test
            anchors.centerIn: parent
            visible: !root.pill
            textFormat: Text.PlainText
            text: root.known ? root.look.glyph : ""
            font: chromeStyle.badgeFont
            color: root.ink
            rotation: root.progressAngle
        }
    }
}
