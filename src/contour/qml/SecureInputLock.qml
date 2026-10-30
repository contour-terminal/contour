// vim:syntax=qml
// The lock a pane shows while the program in it hides what is typed (a password prompt): on its tab, in its
// header, or in a title-band chip when neither is shown (window::SecureIndicatorRules). The native rows draw
// the bundled lock icon, tinted where it sits; the Terminal row sets it as cell text between the chip glyphs
// (`[secure]`). Its accessible name says what it means: "Secure input on" where the OS turns Secure Keyboard
// Entry on, "Input hidden" elsewhere.
import QtQuick
import QtQuick.Controls.impl

Item {
    id: root
    objectName: "secureInputLock"

    /// The ink: the place the lock sits decides it (a header's secondary label, a tab's foreground).
    property color color: chromeStyle.colors.tabText
    /// The face the Terminal row's cell text is set in.
    property font font: chromeStyle.type.footnote

    /// What the lock says about its pane (a window::SecureIndicator's value): "Secure input on" only for
    /// LockedSecured, the pane the OS secures.
    property int indicator: chromeStyle.secureInputs.locked
    /// Whether this row draws the lock as cell text rather than as the icon.
    readonly property bool drawsText: chromeStyle.secureInputCellText !== ""
    /// The icon's edge.
    readonly property real edge: chromeStyle.size.secureInputGlyph

    implicitWidth: drawsText ? cellText.implicitWidth : edge
    implicitHeight: drawsText ? cellText.implicitHeight : edge

    Accessible.role: Accessible.Indicator
    Accessible.name: chromeStyle.secureInputText(root.indicator)

    IconLabel {
        id: icon
        objectName: "secureInputIcon"
        visible: !root.drawsText
        anchors.centerIn: parent
        display: IconLabel.IconOnly
        // The theme's own lock where the desktop has one (Linux), the bundled outline everywhere else.
        icon.name: "changes-prevent-symbolic"
        icon.source: "qrc:/contour/icons/lock-symbolic.svg"
        icon.width: root.edge
        icon.height: root.edge
        icon.color: root.color
    }

    Text {
        id: cellText
        objectName: "secureInputText"
        visible: root.drawsText
        anchors.centerIn: parent
        text: chromeStyle.chipOpenGlyph + chromeStyle.secureInputCellText + chromeStyle.chipCloseGlyph
        font: root.font
        color: root.color
        Accessible.ignored: true
    }
}
