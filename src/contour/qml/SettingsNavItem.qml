// vim:syntax=qml
// One row in the settings page's left navigation rail: a selectable "pill" with an optional leading
// glyph or colour dot, a label, and an optional trailing badge (e.g. the default-profile star or a
// read-only lock). Selection draws a Windows-Terminal-style accent bar on the left over a tinted
// surface; hover shows a fainter tint. Colours follow the OS light/dark theme live via SystemPalette
// (the same idiom as CommandPalette.qml / TabColorFlyout.qml), so the rail adapts in realtime.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ItemDelegate {
    id: root

    property string glyph: ""     ///< Optional leading emoji/glyph (mutually exclusive with the dot).
    property bool showDot: false  ///< Draw a small colour dot instead of a glyph.
    property color dotColor: "gray"
    property string badge: ""     ///< Optional trailing marker (e.g. "★" default, "🔒" read-only).
    property bool selected: false ///< Whether this item is the one currently shown in the content pane.
    property bool accent: false   ///< Colour the label with the accent (used by the "New …" actions).

    implicitHeight: 38
    padding: 0

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
    readonly property color subtleText: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.6)

    background: Item {
        // The selected item reads as a solid surface pill; hover is a fainter accent wash only.
        Rectangle {
            anchors.fill: parent
            radius: 6
            visible: root.selected
            color: sys.base
        }
        Rectangle {
            anchors.fill: parent
            radius: 6
            color: sys.highlight
            opacity: root.selected ? 0.16 : (root.hovered ? 0.08 : 0.0)
            Behavior on opacity { NumberAnimation { duration: 90 } }
        }
        Rectangle {
            visible: root.selected
            width: 3
            radius: 1.5
            color: sys.highlight
            anchors {
                left: parent.left
                top: parent.top
                bottom: parent.bottom
                topMargin: 7
                bottomMargin: 7
            }
        }
    }

    contentItem: RowLayout {
        spacing: 8

        Item { Layout.preferredWidth: 6 } // inset clear of the accent bar

        Label {
            visible: root.glyph.length > 0
            text: root.glyph
            font.pointSize: 11
            Layout.alignment: Qt.AlignVCenter
        }
        Rectangle {
            visible: root.showDot && root.glyph.length === 0
            width: 8
            height: 8
            radius: 4
            color: root.dotColor
            Layout.alignment: Qt.AlignVCenter
        }
        Label {
            text: root.text
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: root.accent ? sys.highlight : sys.windowText
            font.weight: (root.selected || root.accent) ? Font.DemiBold : Font.Normal
            Layout.alignment: Qt.AlignVCenter
        }
        Label {
            visible: root.badge.length > 0
            text: root.badge
            color: root.subtleText
            font.pointSize: 9
            Layout.rightMargin: 8
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
