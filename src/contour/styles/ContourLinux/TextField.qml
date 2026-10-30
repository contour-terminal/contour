// vim:syntax=qml
// TextField for the ContourLinux style: a rounded 34-pixel field on the palette's base, ringed with the
// accent while it has focus. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.TextField {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            Math.max(contentWidth, placeholder.implicitWidth) + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding,
                             placeholder.implicitHeight + topPadding + bottomPadding)

    // Spelled out: T.TextField derives from TextInput, not Control, so the grouped paddings do not exist.
    leftPadding: chromeStyle.space.s4
    rightPadding: chromeStyle.space.s4
    topPadding: 0
    bottomPadding: 0

    font: chromeStyle.font
    color: palette.text
    selectionColor: palette.highlight
    selectedTextColor: palette.highlightedText
    placeholderTextColor: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.5)
    verticalAlignment: TextInput.AlignVCenter

    // How far the placeholder starts past where the first letter will be typed: the caret's width and a
    // hairline, so the caret standing there is not drawn on the placeholder's first letter.
    readonly property real caretClearance: control.cursorRectangle.width + chromeStyle.borderWidth

    // The placeholder hint: T.TextField carries `placeholderText` but draws nothing for it, so a style
    // that does not hands the user an unlabeled empty box. The visibility rule is Controls' own.
    Text {
        id: placeholder
        x: control.leftPadding + control.caretClearance
        y: control.topPadding
        width: control.width - (placeholder.x + control.rightPadding)
        height: control.height - (control.topPadding + control.bottomPadding)

        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        verticalAlignment: control.verticalAlignment
        visible: !control.length && !control.preeditText
                 && (!control.activeFocus || control.horizontalAlignment !== Qt.AlignHCenter)
        elide: Text.ElideRight
        renderType: control.renderType
    }

    background: Rectangle {
        implicitWidth: chromeStyle.minTabWidth
        implicitHeight: chromeStyle.size.control
        radius: chromeStyle.radii.control
        color: control.palette.base
        border.width: chromeStyle.borderWidth
        border.color: control.activeFocus ? control.palette.highlight : control.palette.mid
    }
}
