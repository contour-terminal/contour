// vim:syntax=qml
// MenuSeparator for the ContourLinux style: a hairline rule inset like the rows it parts. See
// ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.MenuSeparator {
    id: control

    implicitWidth: implicitBackgroundWidth + leftInset + rightInset
    implicitHeight: implicitContentHeight + topPadding + bottomPadding

    horizontalPadding: chromeStyle.labelPadding
    verticalPadding: chromeStyle.space.s2

    contentItem: Rectangle {
        implicitHeight: chromeStyle.borderWidth
        color: control.palette.mid
    }
}
