// vim:syntax=qml
// The kit's HostChip: names the host a remote window is attached to (`contour client --host`) and how its
// connection stands -- a status dot, the host, and the state's text ("Reconnecting…", "Disconnected";
// nothing while connected). What each state looks like is UiStyleProvider::hostChipLook's table.
//
// Not interactive: it names where the tabs are, it is not one of them, so it reads as static text. A local
// window has no host and the chip takes no room. On the Terminal row the chip is a run of cells,
// bracketed by the chip glyph tokens: `[● host status]`.
import QtQuick

Item {
    id: root
    objectName: "hostChip"

    /// The attached host; empty for a local window.
    required property string hostLabel
    /// The connection's state, a window::HostChipState as its underlying value. Not `state`, which every
    /// Item already has (the States machinery's name).
    required property int connection

    // How the state looks -- { dot, status, accessibleState } -- re-read on a palette change, since the
    // dot's ink follows it.
    readonly property var look: chromeStyle.paletteRevision < 0 ? ({}) : chromeStyle.hostChipLook(root.connection)
    readonly property string statusText: root.look.status !== undefined ? root.look.status : ""
    // Whether the chip is a run of bracketed cells (the Terminal row) rather than a rounded shape.
    readonly property bool bracketed: chromeStyle.chipOpenGlyph !== ""
    // The room between the dot, the host and the state: one cell on the Terminal row.
    readonly property real gap: root.bracketed ? chromeStyle.cellWidth : chromeStyle.space.s3

    Accessible.role: Accessible.StaticText
    Accessible.name: qsTr("Attached to %1, %2").arg(root.hostLabel).arg(root.look.accessibleState ?? "")

    visible: root.hostLabel !== ""
    implicitWidth: root.visible ? row.implicitWidth + (root.bracketed ? 0 : chromeStyle.space.s5 + chromeStyle.space.s5) : 0
    implicitHeight: chromeStyle.size.hostChip
    width: root.implicitWidth
    height: root.implicitHeight

    Rectangle {
        objectName: "hostChipBackground"
        anchors.fill: parent
        radius: chromeStyle.radii.hostChip
        color: chromeStyle.colors.chipFill
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 0

        Text {
            objectName: "hostChipOpen"
            visible: root.bracketed
            anchors.verticalCenter: parent.verticalCenter
            text: chromeStyle.chipOpenGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.chipText
        }

        // The status dot: a vector circle on the native rows, the dot glyph on the Terminal row.
        Rectangle {
            id: dot
            objectName: "hostChipDot"
            visible: chromeStyle.hostDotGlyph === ""
            anchors.verticalCenter: parent.verticalCenter
            width: chromeStyle.size.hostDot
            height: chromeStyle.size.hostDot
            radius: chromeStyle.size.hostDot
            color: root.look.dot !== undefined ? root.look.dot : "transparent"
        }

        Text {
            objectName: "hostChipDotGlyph"
            visible: chromeStyle.hostDotGlyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: chromeStyle.hostDotGlyph
            font: chromeStyle.type.footnote
            color: dot.color
        }

        Text {
            objectName: "hostChipLabel"
            anchors.verticalCenter: parent.verticalCenter
            // The room after the dot; padding, since a positioner skips an item without height.
            leftPadding: root.gap
            text: root.hostLabel
            font: root.hostFont
            color: chromeStyle.colors.chipTextStrong
        }

        // The state's text, absent while connected.
        Text {
            objectName: "hostChipStatus"
            visible: root.statusText !== ""
            anchors.verticalCenter: parent.verticalCenter
            leftPadding: root.gap
            text: root.statusText
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.chipText
        }

        Text {
            objectName: "hostChipClose"
            visible: root.bracketed
            anchors.verticalCenter: parent.verticalCenter
            text: chromeStyle.chipCloseGlyph
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.chipText
        }
    }

    // The host in the chip's type, bold: a copy, since the token's font is shared.
    readonly property font hostFont: {
        let font = chromeStyle.type.footnote;
        font.bold = true;
        return font;
    }
}
