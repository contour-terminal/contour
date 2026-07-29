// vim:syntax=qml
// Label for the ContourTui style: the chrome font, palette colors. See ToolButton.qml for what a style
// file is and why none of them test for the active style.
import QtQuick
import QtQuick.Templates as T

T.Label {
    color: palette.windowText
    font: chromeStyle.font
    linkColor: palette.link
    verticalAlignment: Text.AlignVCenter
}
