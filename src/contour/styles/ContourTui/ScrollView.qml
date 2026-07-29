// vim:syntax=qml
// ScrollView for the ContourTui style: no frame of its own, leaving the scroll bars (styled by
// ScrollBar.qml) to be the whole of its chrome -- which is what a TUI pane looks like.
// See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.ScrollView {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    T.ScrollBar.vertical: ScrollBar {
        parent: control
        x: control.mirrored ? 0 : control.width - width
        y: control.topPadding
        height: control.availableHeight
        active: control.T.ScrollBar.horizontal.active
    }

    T.ScrollBar.horizontal: ScrollBar {
        parent: control
        x: control.leftPadding
        y: control.height - height
        width: control.availableWidth
        active: control.T.ScrollBar.vertical.active
    }
}
