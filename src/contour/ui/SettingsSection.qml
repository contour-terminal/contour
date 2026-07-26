// vim:syntax=qml
// A titled, collapsible group of settings -- the card the settings page is built out of.
//
// One of these holds the fields of one SettingsController field group (see profileFieldGroups() in
// SettingsController.cpp): a header band with the group's glyph, its title, how many fields it holds and
// a chevron, over a column of the fields themselves. Children declared inside a SettingsSection land in
// that column, so a caller writes what a section contains and never how a section looks.
//
// It does NOT own `expanded`. The page drives that property, because whether a section is open depends
// on things a section cannot see -- an active search forces every matching section open -- and because a
// section's open state has to outlive its delegate, which the search rebuilds on every keystroke. Click
// the header and it emits toggleRequested(); the owner decides what that means. An owner that binds
// `expanded` and ignores the signal gets a section that cannot be collapsed, which is why every call
// site handles it.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property string title: ""
    property string glyph: ""
    /// How many fields the section holds; shown as a badge, hidden when zero.
    property int badgeCount: 0
    /// Whether the body is shown. Owned by the page -- see the note above.
    property bool expanded: true
    /// Emitted when the header is activated by mouse or keyboard.
    signal toggleRequested()

    /// Children declared inside a SettingsSection become rows of its body.
    default property alias content: body.data

    // Live OS palette handle, so the card follows dark/light in realtime (see CommandPalette.qml).
    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
    readonly property color subtleText: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.6)
    readonly property color hairline: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.12)

    radius: 10
    color: sys.base
    border.width: 1
    border.color: root.hairline
    clip: true // so the body does not spill past the rounded corners mid-animation

    // The body is measured but not laid out when collapsed, so a section costs its header height only.
    // Animating this rather than a scale or an opacity keeps the sections below it moving with it.
    implicitHeight: header.implicitHeight + (root.expanded ? body.implicitHeight + 14 : 0)
    Behavior on implicitHeight {
        NumberAnimation { duration: 130; easing.type: Easing.OutCubic }
    }

    // {{{ Header band
    Button {
        id: header
        anchors { left: parent.left; right: parent.right; top: parent.top }
        implicitHeight: 46
        flat: true
        padding: 0

        Accessible.role: Accessible.Button
        Accessible.name: root.title
        Accessible.description: root.expanded ? qsTr("Collapse this group of settings")
                                              : qsTr("Expand this group of settings")
        onClicked: root.toggleRequested()

        background: Rectangle {
            radius: root.radius
            // A wash that deepens on hover. Flat-topped at the bottom edge so an expanded header sits
            // flush against the body instead of showing a rounded seam over it.
            color: sys.highlight
            opacity: header.hovered ? 0.10 : (root.expanded ? 0.05 : 0.0)
            Behavior on opacity { NumberAnimation { duration: 90 } }
            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: parent.radius
                visible: root.expanded
                color: parent.color
            }
        }

        contentItem: RowLayout {
            spacing: 10

            Label {
                Layout.leftMargin: 14
                text: root.glyph
                visible: root.glyph.length > 0
                color: sys.highlight
                font.pointSize: 12
            }
            Label {
                text: root.title
                font.pointSize: 10
                font.weight: Font.DemiBold
                font.letterSpacing: 0.4
                color: sys.windowText
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Rectangle { // count badge
                visible: root.badgeCount > 0
                radius: height / 2
                implicitHeight: 18
                implicitWidth: badgeLabel.implicitWidth + 14
                color: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.09)
                Label {
                    id: badgeLabel
                    anchors.centerIn: parent
                    text: String(root.badgeCount)
                    color: root.subtleText
                    font.pointSize: 8
                }
            }
            Label { // chevron
                Layout.rightMargin: 14
                text: "⌄"
                color: root.subtleText
                font.pointSize: 11
                // Rotated rather than swapped for a second glyph, so the two states are the same shape
                // and the transition reads as one control moving.
                rotation: root.expanded ? 0 : -90
                Behavior on rotation { NumberAnimation { duration: 130; easing.type: Easing.OutCubic } }
            }
        }
    }
    // }}}

    Rectangle { // hairline between header and body
        anchors { left: parent.left; right: parent.right; top: header.bottom }
        height: 1
        visible: root.expanded && body.implicitHeight > 0
        color: root.hairline
    }

    ColumnLayout {
        id: body
        anchors {
            left: parent.left
            right: parent.right
            top: header.bottom
            leftMargin: 12
            rightMargin: 12
            topMargin: 7
        }
        spacing: 2
        // Kept out of the accessibility tree and off the input path while collapsed, so a screen reader
        // and Tab do not walk fields the user cannot see.
        visible: root.expanded
    }
}
