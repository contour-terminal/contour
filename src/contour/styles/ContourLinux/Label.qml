// vim:syntax=qml
// Label for the ContourLinux style: the system UI font, palette colours. See ToolButton.qml for what a
// style file is.
import QtQuick
import QtQuick.Templates as T

T.Label {
    color: palette.windowText
    font: chromeStyle.font
    linkColor: palette.link
    verticalAlignment: Text.AlignVCenter
}
