// vim:syntax=qml
// MenuSeparator for the ContourTui style: a hairline rule inset by one cell -- the horizontal box-rule
// a TUI menu draws between groups. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.MenuSeparator {
    id: control

    implicitWidth: implicitBackgroundWidth + leftInset + rightInset
    implicitHeight: implicitContentHeight + topPadding + bottomPadding

    padding: 0
    horizontalPadding: chromeStyle.labelPadding

    contentItem: Rectangle {
        implicitHeight: chromeStyle.borderWidth
        color: control.palette.mid
    }
}
